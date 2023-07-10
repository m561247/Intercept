#include <codegen/opt/opt-internal.h>

#define ROOT_INLINE_ENTRY ((usz) -1)

typedef struct InlineContext {
  Vector(struct history_entry {
    IRInstruction *call; /// May point to freed memory, don’t dereference.
    IRFunction *callee;  /// The function called by this call.
    usz inlined_via;     /// Index into this history. -1 if root entry.
  }) history;
  Vector(IRInstruction *) not_inlinable;
  isz threshold;
  bool may_fail;
} InlineContext;

/// Compute the number of instructions in a function.
static isz instruction_count(IRFunction *f, bool include_parameters) {
  isz i = 0;
  FOREACH_INSTRUCTION_IN_FUNCTION (f)
    if (include_parameters || (instruction->kind != IR_PARAMETER))
      i++;
  return i;
}

/// Inline a call.
///
/// This will always inline at least one call, if possible, irrespective
/// of the value of the inlining threshold.
///
/// \param cg Codegen context.
/// \param stack Inline stack.
/// \param call The call instruction to inline.
/// \param threshold Inlining threshold in number of instructions for
///        nested calls. If 0, inline everything; if -1, inline only
///        this call.
/// \param Whether this is allowed to fail; if it isn’t, we issue a
///        diagnostic on error.
/// \return True if the call was inlined, false if there was an error.
static bool ir_inline_call(
  CodegenContext *ctx,
  InlineContext *ictx,
  IRInstruction *call
) {
  /// Save the instruction before and after the call.
  IRInstruction *const call_prev = call->prev;
  IRInstruction *const call_next = call->next;
  IRFunction *const callee = call->call.callee_function;
  IRBlock *const call_block = call->parent_block;
  const bool is_tail_call = call->call.tail_call;
  usz call_history_index = 0;

  /// Handle the degenerate case of the callee being empty.
  isz count = instruction_count(callee, true);
  if (count == 0) {
    ASSERT(call->users.size == 0, "Call to empty function cannot possibly return a value");
    ir_remove(call);
    return true;
  }

  /// If the call does not yet exists in the history, add it. If it
  /// does, check if one of its parents is itself.
  {
    struct history_entry *e = NULL;
    foreach_index (i, ictx->history) {
      if (ictx->history.data[i].call == call) {
        e = ictx->history.data + i;
        call_history_index = i;
      }
    }

    /// Call exists.
    if (e) {
      ASSERT(e->inlined_via != ROOT_INLINE_ENTRY);
      e = ictx->history.data + e->inlined_via;
      for (;;) {
        /// If the inlining of this call can be traced back to the inlining
        /// of the same function, then we have an infinite loop.
        if (e->callee == callee) {
          if (!ictx->may_fail) {
            issue_diagnostic(
              DIAG_ERR,
              ctx->ast->filename.data,
              as_span(ctx->ast->source),
              (loc){0},
              "Failed to inline function %S into %S: Infinite loop detected",
              callee->name,
              call->parent_block->function->name
            );
          }

          return false;
        }

        /// Go to our parent’s parent.
        if (e->inlined_via == ROOT_INLINE_ENTRY) break;
        e = ictx->history.data + e->inlined_via;
      }
    }

    /// Call does not exist. Add it as a root to the history. This means
    /// this call was already in the function and wasn’t inlined from
    /// anywhere—at least not in this inlining pass.
    else {
      call_history_index = ictx->history.size;
      vector_push(
        ictx->history,
        (struct history_entry){
          .call = call,
          .callee = callee,
          .inlined_via = ROOT_INLINE_ENTRY,
        }
      );
    }
  }

  /// Remove the call from the list and everything after it
  /// from the block, but leave everything after it connected.
  /// Note that the call cannot be the last instruction in the
  /// block.
  {
    IRInstruction **first = &call_block->instructions.first;
    IRInstruction **last = &call_block->instructions.last;
    if (*first == call || *first == call_next) *first = NULL;
    if (call_prev) call_prev->next = NULL;
    if (call_next) call_next->prev = NULL;
    call->prev = call->next = NULL;
    *last = call_prev;
  }

  /// Copy instructions from the callee into the caller, replacing
  /// any parameter references with the arguments to the call. Since
  /// there may be forward references, we need to create a skeleton
  /// of the IR first, and only then copy all the instruction data.
  ///
  /// This entails allocating as many blocks and IR instructions as
  /// there are in the callee, and then copying the instructions
  /// one by one. This way, we effectively create a mapping of
  /// instructions and blocks of the callee to these instructions
  /// and blocks, which can then be inserted into the caller.
  usz block_count = list_size(callee->blocks);
  Vector(IRInstruction *) instructions = {0};
  Vector(IRBlock *) blocks = {0};
  vector_reserve(instructions, (usz) count);
  vector_reserve(blocks, block_count);

  /// Allocate instructions separately; this is unfortunately necessary
  /// since they will be freed separately later on. The same also applies
  /// to the blocks, except that the first block, i.e. the block into which
  /// we start inserting, is mapped to the block containing the call.
  vector_push(blocks, call_block);
  for (usz i = 0; i < (usz) count; i++) vector_push(instructions, calloc(1, sizeof(IRInstruction)));
  for (usz i = 1; i < block_count; i++) {
    IRBlock *b = calloc(1, sizeof(IRBlock));
    b->function = call_block->function;
    vector_push(blocks, b);
  }

  /// Enumerate instructions.
  usz block_id = 0;
  u32 instruction_id = 0;
  list_foreach (block, callee->blocks) {
    block->id = block_id++;
    list_foreach (inst, block->instructions) {
      if (inst->kind == IR_PARAMETER) continue;
      inst->id = instruction_id++;
    }
  }

  /// Set the ids of any PARAMETER instructions to point to
  /// the arguments of the call. The arguments are mapped
  /// to the last N instructions in the instructions vector.
  foreach_index (param, callee->parameters) {
    u32 mapped_index = (u32) ((usz) count - callee->parameters.size + param);
    callee->parameters.data[param]->id = mapped_index;
    instructions.data[mapped_index] = call->call.arguments.data[param];
  }

  /// Map an instruction or block to its replacement.
#define MAP(inst)        instructions.data[(inst)->id]
#define MAP_BLOCK(block) blocks.data[(block)->id]

  /// PHI and return block in case the callee contains more
  /// than one return instruction and returns a value.
  IRInstruction *return_value = NULL;
  IRBlock *return_block = NULL;

  /// Copy the instructions.
  list_foreach (block, callee->blocks) {
    list_foreach (inst, block->instructions) {
      /// Skip parameters.
      if (inst->kind == IR_PARAMETER) continue;

      /// Copy common data.
      IRInstruction *copy = MAP(inst);
      copy->kind = inst->kind;
      copy->type = inst->type;
      copy->backend_flags = inst->backend_flags;

      /// Copy instruction-specific data.
      STATIC_ASSERT(IR_COUNT == 39, "Handle all instructions in inliner");
      switch (inst->kind) {
        case IR_LIT_INTEGER:
        case IR_LIT_STRING:
        case IR_REGISTER:
        case IR_PARAMETER:
        case IR_COUNT: UNREACHABLE();

        case IR_IMMEDIATE: copy->imm = inst->imm; break;
        case IR_STATIC_REF: copy->static_ref = inst->static_ref; break;
        case IR_FUNC_REF: copy->function_ref = inst->function_ref; break;
        case IR_UNREACHABLE: break;
        case IR_ALLOCA: copy->alloca = inst->alloca; break;

        case IR_INTRINSIC:
          copy->call.intrinsic = inst->call.intrinsic;
          FALLTHROUGH;

        case IR_CALL: {
          copy->call.is_indirect = inst->call.is_indirect;
          copy->call.tail_call = inst->call.tail_call;
          if (inst->call.is_indirect) copy->call.callee_instruction = MAP(inst->call.callee_instruction);
          else copy->call.callee_function = inst->call.callee_function;
          foreach_val (arg, inst->call.arguments)
            vector_push(copy->call.arguments, MAP(arg));

          /// Record the origin of this call.
          if (inst->kind == IR_CALL) {
            vector_push(
              ictx->history,
              (struct history_entry){
                .callee = inst->call.is_indirect ? NULL : inst->call.callee_function,
                .call = copy,
                .inlined_via = call_history_index,
              }
            );
          }
        } break;

        case IR_LOAD:
        case IR_COPY:
        case IR_ZERO_EXTEND:
        case IR_SIGN_EXTEND:
        case IR_TRUNCATE:
        case IR_BITCAST:
        case IR_NOT:
          copy->operand = MAP(inst->operand);
          break;

        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_DIV:
        case IR_MOD:
        case IR_SHL:
        case IR_SAR:
        case IR_SHR:
        case IR_AND:
        case IR_OR:
        case IR_LT:
        case IR_LE:
        case IR_GT:
        case IR_GE:
        case IR_EQ:
        case IR_NE:
          copy->lhs = MAP(inst->lhs);
          copy->rhs = MAP(inst->rhs);
          break;

        case IR_STORE:
          copy->store.value = MAP(inst->store.value);
          copy->store.addr = MAP(inst->store.addr);
          break;

        case IR_BRANCH:
          copy->destination_block = MAP_BLOCK(inst->destination_block);
          break;

        case IR_BRANCH_CONDITIONAL:
          copy->cond_br.condition = MAP(inst->cond_br.condition);
          copy->cond_br.then = MAP_BLOCK(inst->cond_br.then);
          copy->cond_br.else_ = MAP_BLOCK(inst->cond_br.else_);
          break;

        case IR_PHI:
          foreach_index (arg, inst->phi_args) {
            IRPhiArgument *new = calloc(1, sizeof(IRPhiArgument));
            new->value = MAP(inst->phi_args.data[arg]->value);
            new->block = MAP_BLOCK(inst->phi_args.data[arg]->block);
            vector_push(copy->phi_args, new);
          }
          break;

        /// Returns need to be converted to branches to the return
        /// block, and their operands added to the return value phi.
        /// The only exception is if the callee contains only one return
        /// instruction at the very end, in which case we can just inline
        /// it.
        case IR_RETURN: {
          /// If this is a tail call, just emit the return instruction.
          if (is_tail_call) {
            copy->operand = MAP(inst->operand);
            break;
          }

          /// Otherwise, if this is the last return instruction in
          /// the function, and it’s also the only one, just set
          /// the return value and discard it.
          if (!return_block) {
            if (block == callee->blocks.last && inst == block->instructions.last) {
              if (inst->operand) {
                return_value = MAP(inst->operand);
                MAP(inst) = call; /// See below.
              }

              /// Continue because we want to drop this instruction, not insert it.
              continue;
            }

            /// If this is not the last return instruction, we need a
            /// separate return block.
            return_block = ir_block_create();
            if (inst->operand) {
              return_value = calloc(1, sizeof(IRInstruction));
              return_value->kind = IR_PHI;
              ir_insert_into_block(return_block, return_value);
            }
          }

          /// Add to the PHI and branch.
          copy->kind = IR_BRANCH;
          copy->destination_block = return_block;
          if (inst->operand) {
            IRPhiArgument *new = calloc(1, sizeof(IRPhiArgument));
            new->block = block;
            new->value = MAP(inst->operand);
            vector_push(return_value->phi_args, new);

            /// Replace ourselves with the call instruction in
            /// the vector so uses are updated correctly later on.
            MAP(inst) = call;
          }
        } break;
      }

      /// Insert the instruction into the block.
      ir_force_insert_into_block(MAP_BLOCK(block), copy);
    }
  }

  /// Fix up uses.
  FOREACH_INSTRUCTION_IN_FUNCTION (callee) {
    IRInstruction *copy = MAP(instruction);
    foreach_val (user, instruction->users)
      mark_used(copy, MAP(user));
  }

  /// Fix up the return value by replacing all uses of the
  /// call with the return value.
  if (return_value) {
    ir_remove_use(return_value, call);
    ir_replace_uses(call, return_value);
  }

  /// Delete the call.
  ir_remove(call);

  /// To simplify connecting blocks, if we have a return block,
  /// insert it after the last block.
  if (return_block) vector_push(blocks, return_block);

  /// Connect the last inserted instruction with the first
  /// instruction after the call, if any. The last block
  /// may be empty if the callee contains multiple returns
  /// but returns void, in which case we branch here but
  /// at the same time, there is no PHI.
  ///
  /// Note that if the call was a tail call, we just drop
  /// everything after the call.
  IRBlock *last = vector_back(blocks);
  IRInstruction *last_in_last_block = last->instructions.last;
  if (last_in_last_block && !is_tail_call) {
    last_in_last_block->next = call_next;
    if (call_next) call_next->prev = last_in_last_block;
  }

  /// Remove every instruction after the call.
  if (call_next && is_tail_call) {
    IRInstruction *next = call_next;
    while (next) {
      IRInstruction *next_next = next->next;
      ir_unmark_usees(next);
      ir_remove(next);
      next = next_next;
    }
  }

  /// Connect the last block to the previous successor of
  /// the block containing the call instruction.
  if (last != call_block) {
    last->next = call_block->next;
    if (call_block->next) call_block->next->prev = last;

    /// All instructions after the call need to be moved
    /// into this block.
    if (!is_tail_call) {
      for (IRInstruction *i = call_next; i; i = i->next)
        i->parent_block = last;
    }
  }

  /// Connect all new blocks.
  for (usz i = 1; i < block_count; i++) {
    blocks.data[i - 1]->next = blocks.data[i];
    blocks.data[i]->prev = blocks.data[i - 1];
  }

  /// Free unused instructions.
  foreach_val (i, instructions)
    if (!i->parent_block)
      free(i);

  /// Delete vectors.
  vector_delete(instructions);
  vector_delete(blocks);

  /// Done!
  return true;
}

#undef REPLACE

typedef struct {
  bool changed;
  bool failed;
} inline_result;

/// Returns 1 if changed, -1 on error, 0 otherwise.
static inline_result inline_calls_in_function(
  CodegenContext *ctx,
  InlineContext *ictx,
  IRFunction *f
) {
  IRBlock *block = f->blocks.first;
  inline_result res = {0};
  vector_clear(ictx->history);

again:
  for (; block; block = block->next) {
    list_foreach (inst, block->instructions) {
      /// Skip non-calls and indirect calls.
      if (inst->kind != IR_CALL) continue;
      if (inst->call.is_indirect) continue;

      /// Skip calls to external functions.
      IRFunction *callee = inst->call.callee_function;
      if (callee->is_extern) continue;

      /// Skip calls that we’ve already determined are impossible to inline.
      if (vector_contains(ictx->not_inlinable, inst)) continue;

      /// Whether this has to be inlined.
      bool must_inline = callee->attr_forceinline || ictx->threshold == 0;

      /// Inline the call if requested.
      if (must_inline || ictx->threshold >= instruction_count(callee, false)) {
        /// If the callee is the caller, only allow inlining tail calls.
        if (f == callee) {
          if (!inst->call.tail_call) {
            /// If we must inline this call, try to check if it
            /// could be a tail call at least once.
            if (must_inline && !opt_try_convert_to_tail_call(inst)) {
              /// We can’t inline this.
              if (!ictx->may_fail) issue_diagnostic(
                DIAG_ERR,
                ctx->ast->filename.data,
                as_span(ctx->ast->source),
                (loc){0},
                "Sorry, could not inline non-tail-recursive call"
              );
              res.failed = true;
              vector_push(ictx->not_inlinable, inst);
            }
          }

          /// Tail-recursion is better than inlining, so we
          /// leave tail-recursive calls alone.
          continue;
        }

        /// Inline it.
        bool inlined = ir_inline_call(ctx, ictx, inst);
        if (inlined) res.changed = true;
        else {
          res.failed = true;
          vector_push(ictx->not_inlinable, inst);
        }

        /// This may have added new blocks *after* this block,
        /// so start over from the start of this block.
        goto again;
      }
    }
  }

  return res;
}

/// Run the inliner.
static inline_result run_inliner(CodegenContext *ctx, isz threshold, bool may_fail) {
  /// Disjoint-sets datastructure for Kruskal’s algorithm for cycle
  /// detection to make sure we don’t fall into an infinite loop.
  InlineContext ictx = {
    .history = {0},
    .not_inlinable = {0},
    .threshold = threshold,
    .may_fail = may_fail,
  };

  inline_result res = {0};
  foreach_val (f, ctx->functions) {
    inline_result r = inline_calls_in_function(ctx, &ictx, f);
    if (r.failed) res.failed = true;
    if (r.changed) res.changed = true;
  }

  vector_delete(ictx.history);
  vector_delete(ictx.not_inlinable);
  return res;
}

bool opt_inline(CodegenContext *ctx, isz threshold) {
  return run_inliner(ctx, threshold, true).changed;
}

bool codegen_process_inline_calls(CodegenContext *ctx) {
  return !run_inliner(ctx, -1, false).failed;
}
