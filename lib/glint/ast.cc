#include <bit>
#include <glint/ast.hh>
#include <glint/module_description.hh>
#include <glint/parser.hh>
#include <lcc/target.hh>
#include <lcc/utils.hh>
#include <lcc/utils/ast_printer.hh>
#include <lcc/utils/macros.hh>
#include <lcc/utils/rtti.hh>
#include <linux/limits.h>
#include <type_traits>

/// ===========================================================================
///  AST
/// ===========================================================================
lcc::glint::StringLiteral::StringLiteral(
    Module& mod,
    std::string_view value,
    Location location
) : TypedExpr{
    // clang-format off
        Kind::StringLiteral,
        location,
        new(mod) ReferenceType(
            new(mod) ArrayType(
                BuiltinType::Byte(mod),
                new(mod) IntegerLiteral(value.size() + 1, location),
                location
            ),
            location
        ),
    },
    _index{mod.intern(value)} {} // clang-format on

/// Declare a symbol in this scope.
auto lcc::glint::Scope::declare(
    const Context* ctx,
    std::string&& name,
    Decl* decl
) -> Result<Decl*> {
    /// If the symbol already exists, then this is an error, unless
    /// that symbol is a function declaration, and this is also a
    /// function declaration.
    if (
        auto it = symbols.find(name);
        it != symbols.end()
        and not is<FuncDecl>(it->second)
        and not is<FuncDecl>(decl)
    ) return Diag::Error(ctx, decl->location(), "Redeclaration of '{}'", name);

    /// TODO: Check that this declaration is hygienic if it’s part of a macro.

    /// Otherwise, add the symbol.
    symbols.emplace(std::move(name), decl);
    return decl;
}

auto lcc::glint::Expr::type() const -> Type* {
    if (auto e = cast<TypedExpr>(this)) return e->type();
    return Type::Void;
}

auto lcc::glint::Type::align(const lcc::Context* ctx) const -> usz {
    LCC_ASSERT(sema_done_or_errored());
    if (sema_errored()) return 1;
    switch (kind()) {
        case Kind::Builtin:
            switch (as<BuiltinType>(this)->builtin_kind()) {
                using K = BuiltinType::BuiltinKind;
                case K::Bool: return ctx->target()->glint.align_of_bool;
                case K::Byte: return ctx->target()->glint.align_of_byte;
                case K::UInt:
                case K::Int: return ctx->target()->glint.align_of_int;

                /// Alignment must not be 0, so return 1.
                case K::Unknown:
                case K::Void:
                case K::OverloadSet:
                    return 1;
            }
            LCC_UNREACHABLE();

        case Kind::FFIType:
            switch (as<FFIType>(this)->ffi_kind()) {
                using K = FFIType::FFIKind;
                case K::CChar:
                case K::CSChar:
                case K::CUChar:
                    return ctx->target()->ffi.align_of_char;

                case K::CShort:
                case K::CUShort:
                    return ctx->target()->ffi.align_of_short;

                case K::CInt:
                case K::CUInt:
                    return ctx->target()->ffi.align_of_int;

                case K::CLong:
                case K::CULong:
                    return ctx->target()->ffi.align_of_long;

                case K::CLongLong:
                case K::CULongLong:
                    return ctx->target()->ffi.align_of_long_long;
            }
            LCC_UNREACHABLE();

        /// const_cast is ok because we’re just reading the underlying type.
        case Kind::Enum:
            return const_cast<EnumType*>(as<EnumType>(this))->underlying_type()->align(ctx);

        /// Unresolved named type.
        case Kind::Named: return 1;

        /// Functions have no alignment.
        case Kind::Function: return 1;

        case Kind::Pointer:
        case Kind::Reference:
            return ctx->target()->align_of_pointer;

        case Kind::DynamicArray:
            return as<DynamicArrayType>(this)->size(ctx);

        case Kind::Array: return elem()->align(ctx);
        case Kind::Struct: return as<StructType>(this)->alignment();
        case Kind::Integer: return std::bit_ceil(as<IntegerType>(this)->bit_width());
    }

    LCC_UNREACHABLE();
}

auto lcc::glint::Type::elem() const -> Type* {
    switch (kind()) {
        case Kind::Pointer: return as<PointerType>(this)->element_type();
        case Kind::Reference: return as<ReferenceType>(this)->element_type();
        case Kind::Array: return as<ArrayType>(this)->element_type();
        case Kind::DynamicArray: return as<DynamicArrayType>(this)->element_type();

        /// const_cast is ok because we’re just reading the underlying type.
        case Kind::Enum:
            return const_cast<EnumType*>(as<EnumType>(this))->underlying_type();

        case Kind::Builtin:
        case Kind::FFIType:
        case Kind::Named:
        case Kind::Function:
        case Kind::Struct:
        case Kind::Integer:
            Diag::ICE("Type has no element type");
    }
    LCC_UNREACHABLE();
}

namespace {
bool is_builtin(const lcc::glint::Type* t, lcc::glint::BuiltinType::BuiltinKind k) {
    if (auto b = lcc::cast<lcc::glint::BuiltinType>(t)) return b->builtin_kind() == k;
    return false;
}
} // namespace

bool lcc::glint::Type::is_bool() const { return ::is_builtin(this, BuiltinType::BuiltinKind::Bool); }
bool lcc::glint::Type::is_byte() const { return ::is_builtin(this, BuiltinType::BuiltinKind::Byte); }

bool lcc::glint::Type::is_integer(bool include_bool) const {
    return is<IntegerType, FFIType>(this)
        or ::is_builtin(this, BuiltinType::BuiltinKind::UInt)
        or ::is_builtin(this, BuiltinType::BuiltinKind::Int)
        or is_byte()
        or (include_bool and is_bool());
}

bool lcc::glint::Type::is_signed_int(const Context* ctx) const {
    if (auto i = lcc::cast<IntegerType>(this)) return i->is_signed();
    if (auto f = lcc::cast<FFIType>(this)) {
        switch (f->ffi_kind()) {
            using K = FFIType::FFIKind;
            case K::CSChar:
            case K::CShort:
            case K::CInt:
            case K::CLong:
            case K::CLongLong:
                return true;

            case K::CUChar:
            case K::CUShort:
            case K::CUInt:
            case K::CULong:
            case K::CULongLong:
                return false;

            case K::CChar:
                return ctx->target()->ffi.char_is_signed;
        }
    }
    return ::is_builtin(this, BuiltinType::BuiltinKind::Int);
}

bool lcc::glint::Type::is_unknown() const { return ::is_builtin(this, BuiltinType::BuiltinKind::Unknown); }

bool lcc::glint::Type::is_unsigned_int(const Context* ctx) const {
    if (auto i = lcc::cast<IntegerType>(this)) return not i->is_signed();
    if (auto f = lcc::cast<FFIType>(this)) {
        switch (f->ffi_kind()) {
            using K = FFIType::FFIKind;
            case K::CSChar:
            case K::CShort:
            case K::CInt:
            case K::CLong:
            case K::CLongLong:
                return false;

            case K::CUChar:
            case K::CUShort:
            case K::CUInt:
            case K::CULong:
            case K::CULongLong:
                return true;

            case K::CChar:
                return not ctx->target()->ffi.char_is_signed;
        }
    }
    return is_byte();
}

bool lcc::glint::Type::is_void() const { return ::is_builtin(this, BuiltinType::BuiltinKind::Void); }

auto lcc::glint::Type::size(const lcc::Context* ctx) const -> usz {
    LCC_ASSERT(sema_done_or_errored());
    if (sema_errored()) return 0;
    switch (kind()) {
        case Kind::Builtin:
            switch (as<BuiltinType>(this)->builtin_kind()) {
                using K = BuiltinType::BuiltinKind;
                case K::Bool: return ctx->target()->glint.size_of_bool;
                case K::Byte: return ctx->target()->glint.size_of_byte;
                case K::UInt:
                case K::Int: return ctx->target()->glint.size_of_int;

                case K::Unknown:
                case K::Void:
                case K::OverloadSet:
                    return 0;
            }
            LCC_UNREACHABLE();

        case Kind::FFIType:
            switch (as<FFIType>(this)->ffi_kind()) {
                using K = FFIType::FFIKind;
                case K::CChar:
                case K::CSChar:
                case K::CUChar:
                    return ctx->target()->ffi.size_of_char;

                case K::CShort:
                case K::CUShort:
                    return ctx->target()->ffi.size_of_short;

                case K::CInt:
                case K::CUInt:
                    return ctx->target()->ffi.size_of_int;

                case K::CLong:
                case K::CULong:
                    return ctx->target()->ffi.size_of_long;

                case K::CLongLong:
                case K::CULongLong:
                    return ctx->target()->ffi.size_of_long_long;
            }
            LCC_UNREACHABLE();

        /// const_cast is ok because we’re just reading the underlying type.
        case Kind::Enum:
            return const_cast<EnumType*>(as<EnumType>(this))->underlying_type()->size(ctx);

        case Kind::Named: return 0;
        case Kind::Function: return 0;

        case Kind::Pointer:
        case Kind::Reference:
            return ctx->target()->size_of_pointer;

        case Kind::DynamicArray:
            return Type::VoidPtr->size(ctx) + DynamicArrayType::IntegerWidth * 2;

        case Kind::Array:
            return as<ArrayType>(this)->dimension() * elem()->size(ctx);

        case Kind::Struct: return as<StructType>(this)->byte_size() * 8;
        case Kind::Integer: return as<IntegerType>(this)->bit_width();
    }

    LCC_UNREACHABLE();
}

auto lcc::glint::Type::strip_pointers_and_references() -> Type* {
    auto ty = strip_references();
    while (is<PointerType>(ty)) ty = ty->elem();
    return ty;
}

auto lcc::glint::Type::strip_references() -> Type* {
    auto ty = this;
    if (is<ReferenceType>(ty)) ty = ty->elem();
    LCC_ASSERT(not is<ReferenceType>(ty), "Double references are not permitted");
    return ty;
}

bool lcc::glint::Type::Equal(const Type* a, const Type* b) {
    if (a == b) return true;
    if (a->kind() != b->kind()) return false;

    switch (a->kind()) {
        case Kind::Builtin: {
            auto ba = as<BuiltinType>(a);
            auto bb = as<BuiltinType>(b);
            return ba->builtin_kind() == bb->builtin_kind();
        }

        case Kind::FFIType: {
            auto fa = as<FFIType>(a);
            auto fb = as<FFIType>(b);
            return fa->ffi_kind() == fb->ffi_kind();
        }

        /// These are never equal unless they’re the exact same instance.
        case Kind::Named:
        case Kind::Enum:
            return a == b;

        case Kind::Pointer:
        case Kind::Reference:
            return Type::Equal(a->elem(), b->elem());

        case Kind::Array: {
            auto aa = as<ArrayType>(a);
            auto ab = as<ArrayType>(b);
            return aa->dimension() == ab->dimension() and Type::Equal(a->elem(), b->elem());
        }

        case Kind::DynamicArray:
            return Type::Equal(a->elem(), b->elem());

        case Kind::Function: {
            auto fa = as<FuncType>(a);
            auto fb = as<FuncType>(b);

            /// Compare parameters.
            if (fa->params().size() != fb->params().size()) return false;
            for (usz i = 0; i < fa->params().size(); ++i)
                if (not Type::Equal(fa->params()[i].type, fb->params()[i].type))
                    return false;

            /// Compare return type.
            return Type::Equal(fa->return_type(), fb->return_type());
        }

        /// Anonymous structs are equal if their fields have the same
        /// types. Named structs are never equal.
        case Kind::Struct: {
            auto sa = as<StructType>(a);
            auto sb = as<StructType>(b);

            if (sa->decl() or sb->decl()) return false;

            /// Compare fields.
            if (sa->members().size() != sb->members().size()) return false;
            for (usz i = 0; i < sa->members().size(); ++i)
                if (not Type::Equal(sa->members()[i].type, sb->members()[i].type))
                    return false;
            return true;
        }

        case Kind::Integer: {
            auto ia = as<IntegerType>(a);
            auto ib = as<IntegerType>(b);
            return ia->bit_width() == ib->bit_width() and ia->is_signed() == ib->is_signed();
        }
    }

    LCC_UNREACHABLE();
}

auto lcc::glint::ArrayType::dimension() const -> usz {
    LCC_ASSERT(ok(), "Can only call dimension() if type has been type checked successfully");
    return usz(as<ConstantExpr>(size())->value().as_int());
}

// NOTE: DO NOT CALL FOR (T v) COMPOUND LITERALS!
auto lcc::glint::CallExpr::callee_type() const -> FuncType* {
    auto ty = callee()->type();
    while (is<PointerType, ReferenceType>(ty)) ty = ty->elem();
    LCC_ASSERT(ty->is_function());
    return as<FuncType>(ty);
}

auto lcc::glint::Expr::Clone(Module& mod, Expr* expr) -> Expr* {
    LCC_ASSERT(false, "TODO: Clone expressions");
}

auto lcc::glint::EnumeratorDecl::value() const -> aint {
    LCC_ASSERT(ok(), "value() can only be used if the enumerator was analysed successfully");
    return is<ConstantExpr>(init())
             ? as<ConstantExpr>(init())->value().as_int()
             : as<IntegerLiteral>(init())->value();
}

/// ===========================================================================
///  AST Printing
/// ===========================================================================
namespace {
using lcc::as;
using lcc::cast;
using lcc::is;

struct ASTPrinter : lcc::utils::ASTPrinter<ASTPrinter, lcc::glint::Expr, lcc::glint::Type> {
    // NOTE: Can use to override name_colour from ast_printer.hh for Glint names.
    // static constexpr lcc::utils::Colour name_colour{Green};

    // Used to highlight key details, like binary/unary operators, integer literal values, etc.
    static constexpr lcc::utils::Colour key_detail_colour{Red};

    std::unordered_set<const lcc::glint::FuncDecl*> printed_functions{};
    bool print_children_of_children = true;

    void PrintLValue(const lcc::glint::Expr* e) {
        if (e->is_lvalue()) out += fmt::format(" {}lvalue", C(ASTPrinter::base_colour));
    };

    void PrintBasicGlintNode(std::string_view name, const lcc::glint::Expr* node, lcc::glint::Type* t) {
        PrintBasicNode(name, node, t, false);
        PrintLValue(node);
        out += '\n';
    };

    /// Print the header (name + location + type) of a node.
    void PrintHeader(const lcc::glint::Expr* e) {
        using K = lcc::glint::Expr::Kind;
        switch (e->kind()) {
            case K::FuncDecl: {
                auto f = as<lcc::glint::FuncDecl>(e);
                PrintLinkage(f->linkage());
                PrintBasicHeader("FuncDecl", e);
                out += fmt::format(
                    " {}{} {}\n",
                    C(name_colour),
                    f->name(),
                    f->type()->string(use_colour)
                );
                return;
            }

            case K::VarDecl: {
                auto v = as<lcc::glint::VarDecl>(e);
                PrintLinkage(v->linkage());
                PrintBasicHeader("VarDecl", e);
                out += fmt::format(
                    " {}{} {}",
                    C(name_colour),
                    v->name(),
                    v->type()->string(use_colour)
                );
                PrintLValue(e);
                out += '\n';
                return;
            }

            case K::EnumeratorDecl: {
                auto v = as<lcc::glint::EnumeratorDecl>(e);
                PrintBasicHeader("EnumeratorDecl", e);
                out += fmt::format(
                    " {}{} {}{}\n",
                    C(name_colour),
                    v->name(),
                    C(key_detail_colour),
                    v->ok() ? v->value().str() : "?"
                );
                return;
            }
            case K::Binary: {
                auto b = as<lcc::glint::BinaryExpr>(e);
                PrintBasicHeader("BinaryExpr", e);
                out += fmt::format(
                    " {}{} {}",
                    C(key_detail_colour),
                    lcc::glint::ToString(b->op()),
                    b->type()->string(use_colour)
                );
                PrintLValue(e);
                out += '\n';
                return;
            }

            case K::Unary: {
                auto u = as<lcc::glint::UnaryExpr>(e);
                PrintBasicHeader("UnaryExpr", e);
                out += fmt::format(
                    " {}{} {}",
                    C(key_detail_colour),
                    lcc::glint::ToString(u->op()),
                    u->type()->string(use_colour)
                );
                PrintLValue(e);
                out += '\n';
                return;
            }

            case K::IntegerLiteral: {
                auto i = as<lcc::glint::IntegerLiteral>(e);
                PrintBasicHeader("IntegerLiteral", e);
                out += fmt::format(
                    " {}{} {}\n",
                    C(key_detail_colour),
                    i->value(),
                    i->type()->string(use_colour)
                );
                return;
            }

            case K::NameRef: {
                auto n = as<lcc::glint::NameRefExpr>(e);
                PrintBasicHeader("NameRefExpr", e);
                out += fmt::format(
                    " {}{} {}",
                    C(name_colour),
                    n->name(),
                    n->type()->string(use_colour)
                );
                PrintLValue(e);
                out += '\n';
                return;
            }

            case K::Cast: {
                auto c = as<lcc::glint::CastExpr>(e);
                PrintBasicHeader("CastExpr", e);
                switch (c->cast_kind()) {
                    case lcc::glint::CastKind::SoftCast: out += fmt::format(" {}Soft ", C(key_detail_colour)); break;
                    case lcc::glint::CastKind::HardCast: out += fmt::format(" {}Hard ", C(key_detail_colour)); break;
                    case lcc::glint::CastKind::ImplicitCast: out += fmt::format(" {}Implicit ", C(key_detail_colour)); break;
                    case lcc::glint::CastKind::LValueToRValueConv: out += fmt::format(" {}LValueToRValue ", C(key_detail_colour)); break;
                    case lcc::glint::CastKind::LValueToReference: out += fmt::format(" {}LValueToReference ", C(key_detail_colour)); break;
                    case lcc::glint::CastKind::ReferenceToLValue: out += fmt::format(" {}ReferenceToLValue ", C(key_detail_colour)); break;
                }
                out += e->type()->string(use_colour);
                PrintLValue(e);
                out += '\n';
                return;
            }

            case K::If: {
                PrintBasicHeader("IfExpr", e);
                if (not e->type()->is_void()) out += fmt::format(" {}", e->type()->string(use_colour));
                PrintLValue(e);
                out += '\n';
                return;
            }

            case K::OverloadSet: PrintBasicGlintNode("OverloadSet", e, e->type()); return;
            case K::EvaluatedConstant: PrintBasicGlintNode("ConstantExpr", e, e->type()); return;
            case K::Type: PrintBasicGlintNode("TypeExpr", e, e->type()); return;
            case K::TypeDecl: PrintBasicGlintNode("TypeDecl", e, e->type()); return;
            case K::TypeAliasDecl: PrintBasicGlintNode("TypeAliasDecl", e, e->type()); return;
            case K::StringLiteral: PrintBasicGlintNode("StringLiteral", e, e->type()); return;
            case K::CompoundLiteral: PrintBasicGlintNode("CompoundLiteral", e, e->type()); return;
            case K::MemberAccess:
                PrintBasicHeader("MemberAccessExpr", e);

                // Member identifier
                out += fmt::format(
                    " {}.{}",
                    C(name_colour),
                    as<lcc::glint::MemberAccessExpr>(e)->name()
                );

                // Type + lvalue
                out += fmt::format(" {}", e->type()->string(use_colour));
                PrintLValue(e);

                out += '\n';
                return;
            case K::While: PrintBasicGlintNode("WhileExpr", e, nullptr); return;
            case K::For: PrintBasicGlintNode("ForExpr", e, nullptr); return;
            case K::Block: PrintBasicGlintNode("BlockExpr", e, e->type()); return;
            case K::Return: PrintBasicGlintNode("ReturnExpr", e, nullptr); return;
            case K::Call: PrintBasicGlintNode("CallExpr", e, e->type()); return;
            case K::IntrinsicCall: PrintBasicGlintNode("IntrinsicCallExpr", e, e->type()); return;
            case K::Module: PrintBasicGlintNode("ModuleExpr", e, nullptr); return;
            case K::Sizeof: PrintBasicGlintNode("SizeofExpr", e, lcc::glint::Type::Int); return;
            case K::Alignof: PrintBasicGlintNode("AlignofExpr", e, lcc::glint::Type::Int); return;
        }

        PrintBasicGlintNode(R"(<???>)", e, e->type());
    }

    void PrintNodeChildren(const lcc::glint::Expr* e, std::string leading_text = "") {
        if (not print_children_of_children) return;

        /// Print the children of a node.
        using K = lcc::glint::Expr::Kind;
        switch (e->kind()) {
            /// We only print function bodies at the top level.
            case K::FuncDecl: break;

            case K::Binary: {
                auto b = as<lcc::glint::BinaryExpr>(e);
                lcc::glint::Expr* children[] = {b->lhs(), b->rhs()};
                PrintChildren(children, leading_text);
            } break;

            case K::NameRef: {
                auto n = as<lcc::glint::NameRefExpr>(e);
                if (n->target()) {
                    tempset print_children_of_children = false;
                    lcc::glint::Expr* children[] = {n->target()};
                    PrintChildren(children, leading_text);
                }
            } break;

            case K::VarDecl: {
                auto v = as<lcc::glint::VarDecl>(e);
                if (v->init()) {
                    lcc::glint::Expr* children[] = {v->init()};
                    PrintChildren(children, leading_text);
                }
            } break;

            case K::Unary: {
                auto u = as<lcc::glint::UnaryExpr>(e);
                lcc::glint::Expr* children[] = {u->operand()};
                PrintChildren(children, leading_text);
            } break;

            case K::Call: {
                auto c = as<lcc::glint::CallExpr>(e);
                std::vector<lcc::glint::Expr*> children{c->callee()};
                children.insert(children.end(), c->args().begin(), c->args().end());
                PrintChildren(children, leading_text);
            } break;

            case K::Cast: {
                lcc::glint::Expr* children[] = {as<lcc::glint::CastExpr>(e)->operand()};
                PrintChildren(children, leading_text);
            } break;

            case K::CompoundLiteral:
                PrintChildren(as<lcc::glint::CompoundLiteral>(e)->values(), leading_text);
                break;

            case K::While: {
                auto w = as<lcc::glint::WhileExpr>(e);
                lcc::glint::Expr* children[] = {w->condition(), w->body()};
                PrintChildren(children, leading_text);
            } break;

            case K::For: {
                auto f = as<lcc::glint::ForExpr>(e);
                lcc::glint::Expr* children[] = {f->init(), f->condition(), f->increment(), f->body()};
                PrintChildren(children, leading_text);
            } break;

            case K::If: {
                auto i = as<lcc::glint::IfExpr>(e);
                if (i->otherwise()) {
                    lcc::glint::Expr* children[] = {i->condition(), i->then(), i->otherwise()};
                    PrintChildren(children, leading_text);
                } else {
                    lcc::glint::Expr* children[] = {i->condition(), i->then()};
                    PrintChildren(children, leading_text);
                }
            } break;

            case K::Block:
                PrintChildren(as<lcc::glint::BlockExpr>(e)->children(), leading_text);
                break;

            case K::Return: {
                auto ret = as<lcc::glint::ReturnExpr>(e);
                if (ret->value()) PrintChildren({ret->value()}, leading_text);
            } break;

            case K::Sizeof: {
                auto sizeof_expr = as<lcc::glint::SizeofExpr>(e);
                PrintChildren({sizeof_expr->expr()}, leading_text);
            } break;

            case K::Alignof: {
                auto align_expr = as<lcc::glint::AlignofExpr>(e);
                PrintChildren({align_expr->expr()}, leading_text);
            } break;

            case K::MemberAccess: {
                auto member_access_expr = as<lcc::glint::MemberAccessExpr>(e);
                PrintChildren({member_access_expr->object()}, leading_text);
            } break;

            case K::OverloadSet:
            case K::EvaluatedConstant:
            case K::TypeDecl:
            case K::TypeAliasDecl:
            case K::EnumeratorDecl:
            case K::IntegerLiteral:
            case K::StringLiteral:
            case K::IntrinsicCall:
            case K::Module:
            case K::Type:
                break;
        }
    }

    /// Print a top-level node.
    void PrintTopLevelNode(const lcc::glint::Expr* e) {
        PrintHeader(e);
        if (auto f = cast<lcc::glint::FuncDecl>(e)) {
            printed_functions.insert(f);
            if (auto body = const_cast<lcc::glint::FuncDecl*>(f)->body()) {
                if (auto block = cast<lcc::glint::BlockExpr>(body)) {
                    PrintChildren(block->children(), "");
                } else {
                    lcc::glint::Expr* children[] = {const_cast<lcc::glint::FuncDecl*>(f)->body()};
                    PrintChildren(children, "");
                }
            }
        } else {
            PrintNodeChildren(e);
        }
    }

    /// Print a node.
    void operator()(const lcc::glint::Expr* e, std::string leading_text = "") {
        PrintHeader(e);
        PrintNodeChildren(e, std::move(leading_text));
    }

    void print(lcc::glint::Module* mod) {
        printed_functions.insert(mod->top_level_func());
        if (auto funcbody = cast<lcc::glint::BlockExpr>(mod->top_level_func()->body())) {
            for (auto* node : funcbody->children())
                PrintTopLevelNode(node);
        } else PrintTopLevelNode(mod->top_level_func()->body());

        for (auto* f : mod->functions())
            if (not printed_functions.contains(f))
                PrintTopLevelNode(f);
    }
};
} // namespace

auto lcc::glint::Type::string(bool use_colours) const -> std::string {
    static constexpr lcc::utils::Colour type_colour{lcc::utils::Colour::Cyan};

    lcc::utils::Colours C{use_colours};
    using enum lcc::utils::Colour;

    switch (kind()) {
        case Kind::Named: return fmt::format("{}{}", C(White), as<NamedType>(this)->name());
        case Kind::Pointer: {
            /// If the element type of this pointer contains an array or
            /// function type, we need to use parentheses here to preserve
            /// precedence.
            bool has_arr_or_func = false;
            auto el = elem();
            for (;;) {
                switch (el->kind()) {
                    default: break;

                    case Kind::Pointer:
                    case Kind::Reference:
                        el = el->elem();
                        continue;

                    case Kind::Array:
                    case Kind::Function:
                        has_arr_or_func = true;
                        break;
                }
                break;
            }

            return fmt::format(
                "{}{}{}{}{}.ptr{}{}{}",
                C(ASTPrinter::base_colour),
                has_arr_or_func ? "(" : "",
                C(type_colour),
                as<PointerType>(this)->element_type()->string(use_colours),
                C(type_colour),
                C(ASTPrinter::base_colour),
                has_arr_or_func ? ")" : "",
                C(Reset)
            );
        }
        case Kind::Reference: {
            bool has_func = false;
            auto el = elem();
            for (;;) {
                switch (el->kind()) {
                    default: break;

                    case Kind::Pointer:
                    case Kind::Reference:
                        el = el->elem();
                        continue;

                    case Kind::Function:
                        has_func = true;
                        break;
                }
                break;
            }

            return fmt::format(
                "{}{}{}{}{}.ref{}{}{}",
                C(ASTPrinter::base_colour),
                has_func ? "(" : "",
                C(type_colour),
                as<ReferenceType>(this)->element_type()->string(use_colours),
                C(type_colour),
                C(ASTPrinter::base_colour),
                has_func ? ")" : "",
                C(Reset)
            );
        }

        case Kind::Integer: {
            auto i = as<IntegerType>(this);
            return fmt::format("{}{}{}{}", C(type_colour), i->is_signed() ? "s" : "u", i->bit_width(), C(Reset));
        }

        case Kind::Struct: {
            auto decl = as<StructType>(this)->decl();
            return fmt::format(
                "{}struct {}{}",
                C(type_colour),
                not decl or decl->name().empty() ? "<anonymous>" : decl->name(),
                C(Reset)
            );
        }

        case Kind::Enum: {
            auto decl = as<EnumType>(this)->decl();
            return fmt::format(
                "{}enum {}{}",
                C(type_colour),
                not decl or decl->name().empty() ? "<anonymous>" : decl->name(),
                C(Reset)
            );
        }

        case Kind::DynamicArray: {
            auto arr = as<DynamicArrayType>(this);
            return fmt::format(
                "{}[{}{}]{}",
                C(type_colour),
                arr->element_type()->string(use_colours),
                C(type_colour),
                C(Reset)
            );
        }

        case Kind::Array: {
            auto arr = as<ArrayType>(this);
            LCC_ASSERT(arr->size(), "ArrayType has NULL size expression");
            if (auto sz = cast<ConstantExpr>(arr->size())) {
                return fmt::format(
                    "{}[{} {}{}{}]{}",
                    C(type_colour),
                    arr->element_type()->string(use_colours),
                    C(ASTPrinter::name_colour),
                    sz->value().as_int(),
                    C(type_colour),
                    C(Reset)
                );
            }
            return fmt::format(
                "{}[{}{}]{}",
                C(type_colour),
                arr->element_type()->string(use_colours),
                C(type_colour),
                C(Reset)
            );
        }

        case Kind::Builtin:
            switch (as<BuiltinType>(this)->builtin_kind()) {
                using K = BuiltinType::BuiltinKind;
                case K::Bool: return fmt::format("{}bool{}", C(type_colour), C(Reset));
                case K::Byte: return fmt::format("{}byte{}", C(type_colour), C(Reset));
                case K::Int: return fmt::format("{}int{}", C(type_colour), C(Reset));
                case K::UInt: return fmt::format("{}uint{}", C(type_colour), C(Reset));
                case K::Unknown: return fmt::format("{}?{}", C(type_colour), C(Reset));
                case K::Void: return fmt::format("{}void{}", C(type_colour), C(Reset));
                case K::OverloadSet: return fmt::format("{}<overload set>{}", C(type_colour), C(Reset));
            }
            LCC_UNREACHABLE();

        case Kind::FFIType:
            switch (as<FFIType>(this)->ffi_kind()) {
                using K = FFIType::FFIKind;
                case K::CChar: return fmt::format("{}__c_char{}", C(type_colour), C(Reset));
                case K::CSChar: return fmt::format("{}__c_schar{}", C(type_colour), C(Reset));
                case K::CUChar: return fmt::format("{}__c_uchar{}", C(type_colour), C(Reset));
                case K::CShort: return fmt::format("{}__c_short{}", C(type_colour), C(Reset));
                case K::CUShort: return fmt::format("{}__c_ushort{}", C(type_colour), C(Reset));
                case K::CInt: return fmt::format("{}__c_int{}", C(type_colour), C(Reset));
                case K::CUInt: return fmt::format("{}__c_uint{}", C(type_colour), C(Reset));
                case K::CLong: return fmt::format("{}__c_long{}", C(type_colour), C(Reset));
                case K::CULong: return fmt::format("{}__c_ulong{}", C(type_colour), C(Reset));
                case K::CLongLong: return fmt::format("{}__c_longlong{}", C(type_colour), C(Reset));
                case K::CULongLong: return fmt::format("{}__c_ulonglong{}", C(type_colour), C(Reset));
            }
            LCC_UNREACHABLE();

        case Kind::Function: {
            auto f = as<FuncType>(this);
            std::string out = fmt::format("{}{}(", f->return_type()->string(use_colours), C(ASTPrinter::base_colour));
            for (auto& arg : f->params()) {
                if (&arg != &f->params().front()) out += fmt::format("{}, ", C(ASTPrinter::base_colour));
                out += fmt::format("{}{}{}", C(ASTPrinter::name_colour), arg.name, C(ASTPrinter::base_colour));
                if (not arg.name.empty()) out += " : ";
                else out += ":";
                out += arg.type->string(use_colours);
            }
            out += fmt::format("{}){}", C(ASTPrinter::base_colour), C(Reset));
            return out;
        }
    }

    LCC_UNREACHABLE();
}

void lcc::glint::Module::print(bool use_colour) {
    ASTPrinter{use_colour}.print(this);
}
