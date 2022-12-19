#include <codegen.h>

#include <codegen/codegen_forward.h>
#include <codegen/intermediate_representation.h>
#include <codegen/x86_64/arch_x86_64.h>
#include <codegen/ir/ir.h>
#include <environment.h>
#include <error.h>
#include <inttypes.h>
#include <ir_parser.h>
#include <opt.h>
#include <parser.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <typechecker.h>

#define DIAG(sev, loc, ...)                                                                                                   \
  do {                                                                                                                  \
    issue_diagnostic(DIAG_ERR, (ctx)->ast->filename.data, as_span((ctx)->ast->source), (loc), __VA_ARGS__); \
    return false;                                                                                                       \
  } while (0)

#define ERR(...) DIAG(DIAG_ERR, expr->source_location, __VA_ARGS__)

char codegen_verbose = 1;

/// ===========================================================================
///  Context creation.
/// ===========================================================================
CodegenContext *codegen_context_create
(AST *ast,
 enum CodegenOutputFormat format,
 enum CodegenCallingConvention call_convention,
 enum CodegenAssemblyDialect dialect,
 FILE* code
 )
{
  CodegenContext *context;

  STATIC_ASSERT(CG_FMT_COUNT == 2, "codegen_context_create_top_level() must exhaustively handle all codegen output formats.");
  STATIC_ASSERT(CG_CALL_CONV_COUNT == 2, "codegen_context_create_top_level() must exhaustively handle all calling conventions.");
  switch (format) {
    case CG_FMT_x86_64_GAS:
      // TODO: Handle call_convention for creating codegen context!
      if (call_convention == CG_CALL_CONV_MSWIN) {
        context = codegen_context_x86_64_mswin_create(NULL);
      } else if (call_convention == CG_CALL_CONV_LINUX) {
        context = codegen_context_x86_64_linux_create(NULL);
      } else {
        PANIC("Unrecognized calling convention!");
      }
      break;
    case CG_FMT_IR:
      context = codegen_context_ir_create(NULL);
      break;
    default: UNREACHABLE();
  }

  context->ast = ast;
  context->code = code;
  context->dialect = dialect;
  context->functions = calloc(1, sizeof *context->functions);
  return context;
}

void codegen_context_free(CodegenContext *context) {
  STATIC_ASSERT(CG_FMT_COUNT == 2, "codegen_context_free() must exhaustively handle all codegen output formats.");
  STATIC_ASSERT(CG_CALL_CONV_COUNT == 2, "codegen_context_free() must exhaustively handle all calling conventions.");

  VECTOR_DELETE(*context->functions);

  switch (context->format) {
    default: UNREACHABLE();
    case CG_FMT_x86_64_GAS:
      if (context->call_convention == CG_CALL_CONV_MSWIN) {
        return codegen_context_x86_64_mswin_free(context);
      } else if (context->call_convention == CG_CALL_CONV_LINUX) {
        // return codegen_context_x86_64_gas_linux_free(parent);
      }
      break;
    case CG_FMT_IR:
      return codegen_context_ir_free(context);
  }
}

/// ===========================================================================
///  Code generation.
/// ===========================================================================
/// Emit an expression.
static void codegen_expr(CodegenContext *ctx, Node *expr) {
  switch (expr->kind) {
  default: ICE("Unrecognized expression kind: %d", expr->kind);

  /// Types don’t need to be emitted.
  case NODE_TYPE_PRIMITIVE:
  case NODE_TYPE_NAMED:
  case NODE_TYPE_POINTER:
  case NODE_TYPE_ARRAY:
  case NODE_TYPE_FUNCTION:
    break;

  /// A function returns its address. This has already been taken care of.
  case NODE_FUNCTION: break;

  /// Root node.
  case NODE_ROOT: {
    /// Main function.
    ir_function(ctx, "main", 2);

    /// TODO: ‘declare’ all functions in the IR and add IRFunction refs to the function syms.

    /// Emit everything that isn’t a function.
    Node *last = NULL;
    VECTOR_FOREACH_PTR (Node *, child, expr->root.children) {
      if (child->kind == NODE_FUNCTION) continue;
      last = child;
      codegen_expr(ctx, child);
    }

    /// If the last expression doesn’t return anything, return 0.
    if (!ir_is_closed(ctx->block)) ir_return(ctx, last ? last->ir : ir_immediate(ctx, 0));
    return;
  }

  /// Variable declaration.
  case NODE_DECLARATION:
      expr->ir = ir_stack_allocate(ctx, ast_sizeof(expr));
      return;

  /// If expression.
  ///
  /// Each box is a basic block within intermediate representation,
  /// and edges represent control flow from top to bottom.
  ///
  ///      +---------+
  ///      | current |
  ///      +---------+
  ///     /           \
  /// +------+    +------+
  /// | then |    | else |
  /// +------+    +------+
  ///         \  /
  ///       +------+
  ///       | join |
  ///       +------+
  ///
  case NODE_IF: {
    /// Emit the condition.
    codegen_expr(ctx, expr->if_.condition);

    IRBlock *then_block = ir_block_create();
    IRBlock *last_then_block = then_block;
    IRBlock *else_block = ir_block_create();
    IRBlock *last_else_block = else_block;
    IRBlock *join_block = ir_block_create();

    /// Generate the branch.
    ir_branch_conditional(ctx, expr->if_.condition->ir, then_block, else_block);

    /// Emit the then block.
    ir_block_attach(ctx, then_block);
    codegen_expr(ctx, expr->if_.then);

    /// Branch to the join block to skip the else branch.
    last_then_block = ctx->block;
    if (!ir_is_closed(ctx->block)) ir_branch(ctx, join_block);

    /// Generate the else block if there is one.
    ir_block_attach(ctx, else_block);
    if (expr->if_.else_) {
      codegen_expr(ctx, expr->if_.else_);
      last_else_block = ctx->block;
    } else {
      ir_immediate(ctx, 0);
    }

    /// Branch to the join block from the else branch.
    if (!ir_is_closed(ctx->block)) ir_branch(ctx, join_block);

    /// This assumes that the last instruction in a block returns a
    /// value; if it doesn't, we will simply return zero. This should
    /// probably be ensured in the type checker in the future.
    /// TODO: This should actually be the last instruction *that actually yields a value*.
    ///
    /// NOTE: `block->instructions.last->prev` is the last instruction in `block`
    /// before the branch.
    IRInstruction *then_return_value = last_then_block->instructions.last->prev
      ? last_then_block->instructions.last->prev
      : ir_immediate(ctx, 0);
    IRInstruction *else_return_value = last_else_block->instructions.last->prev
      ? last_else_block->instructions.last->prev
      : ir_immediate(ctx, 0);

    /// Attach the join block.
    ir_block_attach(ctx, join_block);

    /// Insert a phi node for the result of the if in the join block.
    IRInstruction *phi = ir_phi(ctx);
    ir_phi_argument(phi, last_then_block, then_return_value);
    ir_phi_argument(phi, last_else_block, else_return_value);
    expr->ir = phi;
    return;
  }

  /// While expression.
  ///
  /// +---------+
  /// | current |
  /// +---------+        ,---------+
  ///      |             |         |
  /// +--------------------+       |
  /// | compute condition  |       |
  /// | conditional branch |       |
  /// +--------------------+       |
  ///      |             |         |
  ///      |      +------------+   |
  ///      |      | body       |   |
  ///      |      +------------+   |
  ///      |             |         |
  ///      |            ...        |
  ///      |             |         |
  ///  +----------+      `---------+
  ///  | join     |
  ///  +----------+
  case NODE_WHILE: {
      IRBlock *while_cond_block = ir_block_create();
      IRBlock *join_block = ir_block_create();

      /// Branch to the condition block and emit the condition.
      ir_branch(ctx, while_cond_block);
      ir_block_attach(ctx, while_cond_block);
      codegen_expr(ctx, expr->while_.condition);

      /// If while body is empty, don't use body block.
      if (expr->while_.body->block.children.size == 0) {
        ir_branch_conditional(ctx, expr->while_.condition->ir, while_cond_block, join_block);
        ir_block_attach(ctx, join_block);
        return;
      }

      /// Otherwise, emit the body of the while loop.
      IRBlock *while_body_block = ir_block_create();
      ir_branch_conditional(ctx, expr->while_.condition->ir, while_body_block, join_block);
      ir_block_attach(ctx, while_body_block);
      codegen_expr(ctx, expr->while_.body);

      /// Emit a branch to the join block and attach the join block.
      if (!ir_is_closed(ctx->block)) ir_branch(ctx, while_cond_block);
      ir_block_attach(ctx, join_block);
      return;
  }

  /// Block expression.
  case NODE_BLOCK: {
      /// Emit everything that isn’t a function.
      Node *last = NULL;
      VECTOR_FOREACH_PTR (Node *, child, expr->block.children) {
        if (child->kind == NODE_FUNCTION) continue;
        last = child;
        codegen_expr(ctx, child);
      }

      /// If the last expression doesn’t return anything, return 0.
      expr->ir = last ? last->ir : ir_immediate(ctx, 0);
      return;
  }

  /// Function call.
  case NODE_CALL: {
    IRInstruction *call = NULL;

    /// Direct call.
    if (expr->call.callee->kind == NODE_TYPE_FUNCTION) {
      call = ir_direct_call(ctx, expr->call.callee->function.ir);
    }

    /// Indirect call.
    else {
      codegen_expr(ctx, expr->call.callee);
      call = ir_indirect_call(ctx, expr->call.callee->ir);
    }

    /// Emit the arguments.
    VECTOR_FOREACH_PTR (Node*, arg, expr->call.arguments) {
      codegen_expr(ctx, arg);
      ir_add_function_call_argument(ctx, call, arg->ir);
    }

    ir_insert(ctx, call);
    expr->ir = call;
    return;
  }

  /// Typecast.
  case NODE_CAST: { TODO(); }

  /// Binary expression.
  case NODE_BINARY: {
    Node * const lhs = expr->binary.lhs, * const rhs = expr->binary.rhs;

    /// Assignment needs to be handled separately.
    if (expr->binary.op == TK_COLON_EQ) {
      /// Emit the RHS because we need that in any case.
      codegen_expr(ctx, rhs);

      /// If the LHS is a variable, just emit a direct store to the memory address,
      /// which we can get from the declaration which has to have been emitted already.
      if (lhs->kind == NODE_VARIABLE_REFERENCE) {
        expr->ir = ir_store(ctx, lhs->var->value->ir, rhs->ir);
        return;
      }

      /// Otherwise, actually emit the LHS and load from that.
      codegen_expr(ctx, lhs);
      expr->ir = ir_store(ctx, lhs->ir, rhs->ir);
      return;
    }

    /// Emit the operands.
    codegen_expr(ctx, lhs);
    codegen_expr(ctx, rhs);

    /// Emit the binary instruction.
    switch (expr->binary.op) {
      default: ICE("Cannot emit binary expression of type %d", expr->binary.op);
      case TK_LBRACK: expr->ir = ir_add(ctx, lhs->ir, rhs->ir); return;
      case TK_GT: expr->ir = ir_comparison(ctx, COMPARE_GT, lhs->ir, rhs->ir); return;
      case TK_LT: expr->ir = ir_comparison(ctx, COMPARE_LT, lhs->ir, rhs->ir); return;
      case TK_GE: expr->ir = ir_comparison(ctx, COMPARE_GE, lhs->ir, rhs->ir); return;
      case TK_LE: expr->ir = ir_comparison(ctx, COMPARE_LE, lhs->ir, rhs->ir); return;
      case TK_EQ: expr->ir = ir_comparison(ctx, COMPARE_EQ, lhs->ir, rhs->ir); return;
      case TK_NE: expr->ir = ir_comparison(ctx, COMPARE_NE, lhs->ir, rhs->ir); return;
      case TK_PLUS: expr->ir = ir_add(ctx, lhs->ir, rhs->ir); return;
      case TK_MINUS: expr->ir = ir_subtract(ctx, lhs->ir, rhs->ir); return;
      case TK_STAR: expr->ir = ir_multiply(ctx, lhs->ir, rhs->ir); return;
      case TK_SLASH: expr->ir = ir_divide(ctx, lhs->ir, rhs->ir); return;
      case TK_PERCENT: expr->ir = ir_modulo(ctx, lhs->ir, rhs->ir); return;
      case TK_SHL: expr->ir = ir_shift_left(ctx, lhs->ir, rhs->ir); return;
      case TK_SHR: expr->ir = ir_shift_right_arithmetic(ctx, lhs->ir, rhs->ir); return;
      case TK_AMPERSAND: expr->ir = ir_and(ctx, lhs->ir, rhs->ir); return;
      case TK_PIPE: expr->ir = ir_or(ctx, lhs->ir, rhs->ir); return;
    }
  }

  /// Unary expression.
  case NODE_UNARY: {
    codegen_expr(ctx, expr->unary.value);

    /// Prefix expressions.
    if (!expr->unary.postfix) {
      switch (expr->unary.op) {
        default: ICE("Cannot emit unary prefix expression of type %d", expr->unary.op);

        /// Load a value from an lvalue.
        case TK_AT: expr->ir = ir_load(ctx, expr->unary.value->ir); return;

        /// Address of lvalue.
        case TK_AMPERSAND: expr->ir = ir_address_of(ctx, expr->unary.value->ir); return;

        /// One’s complement negation.
        case TK_TILDE: expr->ir = ir_not(ctx, expr->unary.value->ir); return;
      }
    }

    /// Postfix expressions.
    else {
      switch (expr->unary.op) {
        default: ICE("Cannot emit unary postfix expression of type %d", expr->unary.op);
      }
    }
  }

  /// Literal expression. Only integer literals are supported for now.
  case NODE_LITERAL:
    if (expr->literal.type != TK_NUMBER) DIAG(DIAG_SORRY, expr->source_location, "Emitting non-integer literals not supported");
    expr->ir = ir_immediate(ctx, expr->literal.integer);
    return;

  /// Variable reference.
  case NODE_VARIABLE_REFERENCE:
    expr->ir = ir_load(ctx, expr->var->value->ir);
    return;

  /// Function reference.
  case NODE_FUNCTION_REFERENCE:
    expr->ir = ir_address_of_function(ctx, expr->funcref->value->function.ir);
    return;
  }
}

/// Emit a function.
void codegen_function(CodegenContext *ctx, Node *node) {
  /// Create a new function.
  ir_function(ctx, as_span(node->function.name), node->function.type->type_function.parameters.size);

  /// The first N nodes in the body of a function, where N is the number
  /// of parameters, are variable declarations that correspond to the
  /// parameters. First, emit all of them.
  size_t i = 0;
  for (; i < node->function.type->type_function.parameters.size; i++) {
    Node * decl = node->function.body->block.children.data[i];
    codegen_expr(ctx, decl);
  }

  /// Then, store the initial values of the parameters in those variables.
  VECTOR_FOREACH_INDEX (j, node->function.type->type_function.parameters) {
    /// Get each parameter value and store in its corresponding local variable.
    IRInstruction *p = ir_parameter(ctx, j);
    ir_store_local(ctx, p, node->function.body->block.children.data[i]->ir);
  }

  /// Emit the rest of the function body.
  Node *last = NULL;
  for (; i < node->function.body->block.children.size; i++) {
    Node *expr = node->function.body->block.children.data[i];
    if (expr->kind == NODE_FUNCTION) continue;
    last = expr;
    codegen_expr(ctx, expr);
  }

  /// If the last expression doesn’t return anything, return 0.
  if (!ir_is_closed(ctx->block)) ir_return(ctx, last ? last->ir : ir_immediate(ctx, 0));
  return;
}

/// ===========================================================================
///  Driver
/// ===========================================================================
void codegen_lower(CodegenContext *context) {
  switch (context->format) {
    case CG_FMT_x86_64_GAS:
      codegen_lower_x86_64(context);
      break;
    case CG_FMT_IR:
      codegen_lower_ir_backend(context);
      break;
    default:
      TODO("Handle %d code generation format.", context->format);
  }
}

void codegen_emit(CodegenContext *context) {
  switch (context->format) {
    case CG_FMT_x86_64_GAS:
      codegen_emit_x86_64(context);
      break;
    case CG_FMT_IR:
      codegen_emit_ir_backend(context);
      break;
    default:
      TODO("Handle %d code generation format.", context->format);
  }
}

bool codegen
(enum CodegenLanguage lang,
 enum CodegenOutputFormat format,
 enum CodegenCallingConvention call_convention,
 enum CodegenAssemblyDialect dialect,
 const char *infile,
 const char *outfile,
 AST *ast,
 string ir
 )
{
  if (!outfile) ICE("codegen(): outfile can not be NULL!");
  // Open file for writing.
  FILE *code = fopen(outfile, "w");
  if (!code) ICE("codegen(): failed to open file at path: \"%s\"\n", outfile);

  CodegenContext *context = codegen_context_create(ast, format, call_convention, dialect, code);
  switch (lang) {
    /// Parse an IR file.
    case LANG_IR: {
        if (!ir_parse(context, infile, ir)) {
          fclose(code);
          return false;
        }
    } break;

    /// Codegen a FUN program.
    case LANG_FUN: codegen_expr(context, ast->root); break;

    /// Anything else is not supported.
    default: ICE("Language %d not supported.", lang);
  }

  if (optimise) codegen_optimise(context);

  codegen_lower(context);

  if (debug_ir) {
    printf("Backend Lowered\n");
    ir_femit(stdout, context);
  }

  codegen_emit(context);

  codegen_context_free(context);

  fclose(code);
  return true;
}

