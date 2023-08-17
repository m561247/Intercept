#include <bit>
#include <intercept/ast.hh>
#include <intercept/parser.hh>
#include <lcc/target.hh>
#include <lcc/utils/ast_printer.hh>
#include <lcc/utils/rtti.hh>

namespace intc = lcc::intercept;

/// ===========================================================================
///  Module
/// ===========================================================================
intc::Module::Module(
    File* file,
    std::string module_name,
    bool is_logical_module
) : name{std::move(module_name)}, is_module{is_logical_module}, file{file} {
    FuncType* ty{};

    /// Create the type of the top-level function.
    if (is_logical_module) {
        ty = new (*this) FuncType({}, BuiltinType::Void(*this), {}, {});
    } else {
        auto cchar_ty = FFIType::CChar(*this);
        auto cint_ty = FFIType::CInt(*this);
        auto char_ptr = new (*this) PointerType{new (*this) PointerType{cchar_ty}};
        ty = new (*this) FuncType{
            {
                {"__argc__", cint_ty, {}},
                {"__argv__", char_ptr, {}},
                {"__envp__", char_ptr, {}},
            },
            cint_ty,
            {},
            {},
        };
    }

    /// FIXME: What name are we using for module initialisers again?
    top_level_function = new (*this) FuncDecl{
        is_logical_module ? fmt::format(".init.{}", name) : "main",
        ty,
        new (*this) BlockExpr{{}, {}},
        this,
        Linkage::Exported,
        {},
    };
}

intc::Module::~Module() {
    for (auto* node : nodes) delete node;
    for (auto* type : types) delete type;
    for (auto* scope : scopes) delete scope;
    for (auto& [_, i] : _imports) delete i;
}

void lcc::intercept::Module::add_top_level_expr(Expr* node) {
    as<BlockExpr>(top_level_function->body())->add(node);
}

auto intc::Module::intern(std::string_view str) -> usz {
    auto it = rgs::find(strings, str);
    if (it != strings.end()) { return usz(it - strings.begin()); }
    strings.emplace_back(str);
    return strings.size() - 1;
}

/// ===========================================================================
///  AST
/// ===========================================================================
intc::StringLiteral::StringLiteral(
    Module& mod,
    std::string_view value,
    Location location
) : TypedExpr{
        Kind::StringLiteral,
        location,
        new(mod) ArrayType(
            BuiltinType::Byte(mod),
            new(mod) IntegerLiteral(value.size(), location),
            location
        ),
    },
    _index{mod.intern(value)} {
}

/// Declare a symbol in this scope.
auto intc::Scope::declare(
    Parser* p,
    std::string&& name,
    Decl* decl
) -> Result<Decl*> {
    /// If the symbol already exists, then this is an error, unless
    /// that symbol is a function declaration, and this is also a
    /// function declaration.
    if (
        auto it = symbols.find(name);
        it != symbols.end() and
        not is<FuncDecl>(it->second) and
        not is<FuncDecl>(decl)
    ) return Diag::Error(p->context, decl->location(), "Redeclaration of '{}'", name);

    /// TODO: Check that this declaration is hygienic if it’s part of a macro.

    /// Otherwise, add the symbol.
    symbols.emplace(std::move(name), decl);
    return decl;
}

bool intc::Expr::is_lvalue() const {
    return is<ReferenceType>(type()) or is<VarDecl, FuncDecl>(this);
}

bool intc::Expr::is_assignable_lvalue() const {
    /// References to anything other than functions are
    /// assignable lvalues.
    if (auto ref = cast<ReferenceType>(type()))
        return not is<FuncType>(ref->element_type());

    /// Variable declarations are assignable lvalues.
    return is<VarDecl>(this);
}

auto intc::Expr::type() const -> Type* {
    if (auto e = cast<TypedExpr>(this)) return e->type();
    return Type::Void;
}

auto intc::Type::align(const lcc::Context* ctx) const -> usz {
    LCC_ASSERT(sema_done_or_errored());
    if (sema_errored()) return 1;
    switch (kind()) {
        case Kind::Builtin:
            switch (as<BuiltinType>(this)->builtin_kind()) {
                using K = BuiltinType::BuiltinKind;
                case K::Bool: return ctx->target()->intercept.align_of_bool;
                case K::Byte: return ctx->target()->intercept.align_of_byte;
                case K::Int: return ctx->target()->intercept.align_of_int;

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

        /// Unresolved named type.
        case Kind::Named: return 1;

        /// Functions have no alignment.
        case Kind::Function: return 1;

        case Kind::Pointer:
        case Kind::Reference:
            return ctx->target()->align_of_pointer;

        case Kind::Array: return elem()->align(ctx);
        case Kind::Struct: return as<StructType>(this)->alignment();
        case Kind::Integer: return std::bit_ceil(as<IntegerType>(this)->bit_width());
    }

    LCC_UNREACHABLE();
}

auto intc::Type::elem() const -> Type* {
    switch (kind()) {
        case Kind::Pointer: return as<PointerType>(this)->element_type();
        case Kind::Reference: return as<ReferenceType>(this)->element_type();
        case Kind::Array: return as<ArrayType>(this)->element_type();

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
bool is_builtin(const intc::Type* t, intc::BuiltinType::BuiltinKind k) {
    if (auto b = lcc::as<intc::BuiltinType>(t)) return b->builtin_kind() == k;
    return false;
}
} // namespace

bool intc::Type::is_bool() const { return ::is_builtin(this, BuiltinType::BuiltinKind::Bool); }
bool intc::Type::is_byte() const { return ::is_builtin(this, BuiltinType::BuiltinKind::Byte); }

bool intc::Type::is_integer(bool include_bool) const {
    return is<IntegerType, FFIType>(this) or
           ::is_builtin(this, BuiltinType::BuiltinKind::Int) or
           is_byte() or
           (include_bool and is_bool());
}

bool intc::Type::is_signed_int(const Context* ctx) const {
    if (auto i = lcc::as<IntegerType>(this)) return i->is_signed();
    if (auto f = lcc::as<FFIType>(this)) {
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

bool intc::Type::is_unknown() const { return ::is_builtin(this, BuiltinType::BuiltinKind::Unknown); }

bool intc::Type::is_unsigned_int(const Context* ctx) const {
    if (auto i = lcc::as<IntegerType>(this)) return not i->is_signed();
    if (auto f = lcc::as<FFIType>(this)) {
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

bool intc::Type::is_void() const { return ::is_builtin(this, BuiltinType::BuiltinKind::Void); }

auto intc::Type::size(const lcc::Context* ctx) const -> usz {
    LCC_ASSERT(sema_done_or_errored());
    if (sema_errored()) return 0;
    switch (kind()) {
        case Kind::Builtin:
            switch (as<BuiltinType>(this)->builtin_kind()) {
                using K = BuiltinType::BuiltinKind;
                case K::Bool: return ctx->target()->intercept.size_of_bool;
                case K::Byte: return ctx->target()->intercept.size_of_byte;
                case K::Int: return ctx->target()->intercept.size_of_int;

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

        case Kind::Named: return 0;
        case Kind::Function: return 0;

        case Kind::Pointer:
        case Kind::Reference:
            return ctx->target()->size_of_pointer;

        case Kind::Array: {
            EvalResult res;
            LCC_ASSERT(as<ArrayType>(this)->size()->evaluate(res, true), "Ill-formed array type");
            auto size = res.as_i64();
            return usz(size) * elem()->size(ctx);
        }

        case Kind::Struct: return as<StructType>(this)->byte_size() * 8;
        case Kind::Integer: return as<IntegerType>(this)->bit_width();
    }

    LCC_UNREACHABLE();
}

auto intc::Type::strip_pointers_and_references() -> Type* {
    auto ty = this;
    while (is<PointerType, ReferenceType>(ty)) ty = ty->elem();
    return ty;
}

auto intc::Type::strip_references() -> Type* {
    auto ty = this;
    while (is<ReferenceType>(ty)) ty = ty->elem();
    return ty;
}

bool intc::Type::Equal(const Type* a, const Type* b) {
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

        /// Named types are never equal unless they’re the exact same instance.
        case Kind::Named: return a == b;

        case Kind::Pointer:
        case Kind::Reference:
            return Type::Equal(a->elem(), b->elem());

        case Kind::Array: {
            EvalResult a_sz, b_sz;
            LCC_ASSERT(
                as<ArrayType>(a)->size()->evaluate(a_sz, true) and
                    as<ArrayType>(b)->size()->evaluate(b_sz, true),
                "Ill-formed array types"
            );
            return a_sz.as_i64() == b_sz.as_i64() and Type::Equal(a->elem(), b->elem());
        }

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
        } break;

        case Kind::Integer: {
            auto ia = as<IntegerType>(a);
            auto ib = as<IntegerType>(b);
            return ia->bit_width() == ib->bit_width() and ia->is_signed() == ib->is_signed();
        }
    }

    LCC_UNREACHABLE();
}

auto intc::Expr::Clone(Module& mod, Expr* expr) -> Expr* {
    LCC_ASSERT(false, "TODO: Clone expressions");
}

/// ===========================================================================
///  AST Printing
/// ===========================================================================
namespace {
using lcc::as;
using lcc::cast;
using lcc::is;

struct ASTPrinter : lcc::utils::ASTPrinter<ASTPrinter, intc::Expr, intc::Type> {
    /// Print the header (name + location + type) of a node.
    void PrintHeader(const intc::Expr* e) {
        using K = intc::Expr::Kind;
        switch (e->kind()) {
            case K::FuncDecl: {
                auto f = as<intc::FuncDecl>(e);
                PrintLinkage(f->linkage());
                PrintBasicHeader("FuncDecl", e);
                out += fmt::format(
                    " {}{} {}\n",
                    C(Green),
                    f->name(),
                    f->type()->string(use_colour)
                );
                return;
            }

            case K::VarDecl: {
                auto v = as<intc::VarDecl>(e);
                PrintLinkage(v->linkage());
                PrintBasicHeader("VarDecl", e);
                out += fmt::format(
                    " {}{} {}\n",
                    C(White),
                    v->name(),
                    v->type()->string(use_colour)
                );
                return;
            }

            case K::Binary: {
                auto b = as<intc::BinaryExpr>(e);
                PrintBasicHeader("BinaryExpr", e);
                out += fmt::format(
                    " {}{} {}\n",
                    C(Red),
                    intc::ToString(b->op()),
                    b->type()->string(use_colour)
                );
                return;
            }

            case K::Unary: {
                auto u = as<intc::UnaryExpr>(e);
                PrintBasicHeader("UnaryExpr", e);
                out += fmt::format(
                    " {}{} {}\n",
                    C(Red),
                    intc::ToString(u->op()),
                    u->type()->string(use_colour)
                );
                return;
            }

            case K::IntegerLiteral: {
                auto i = as<intc::IntegerLiteral>(e);
                PrintBasicHeader("IntegerLiteral", e);
                out += fmt::format(
                    " {}{} {}\n",
                    C(Magenta),
                    i->value(),
                    i->type()->string(use_colour)
                );
                return;
            }

            case K::NameRef: {
                auto n = as<intc::NameRefExpr>(e);
                PrintBasicHeader("NameRefExpr", e);
                out += fmt::format(
                    " {}{} {}\n",
                    C(White),
                    n->name(),
                    n->type()->string(use_colour)
                );
                return;
            }

            case K::OverloadSet: PrintBasicNode("OverloadSet", e, e->type()); return;
            case K::EvaluatedConstant: PrintBasicNode("ConstantExpr", e, e->type()); return;
            case K::StructDecl: PrintBasicNode("StructDecl", e, e->type()); return;
            case K::TypeAliasDecl: PrintBasicNode("TypeAliasDecl", e, e->type()); return;
            case K::StringLiteral: PrintBasicNode("StringLiteral", e, e->type()); return;
            case K::CompoundLiteral: PrintBasicNode("CompoundLiteral", e, e->type()); return;
            case K::If: PrintBasicNode("IfExpr", e, e->type()); return;
            case K::While: PrintBasicNode("WhileExpr", e, nullptr); return;
            case K::For: PrintBasicNode("ForExpr", e, nullptr); return;
            case K::Block: PrintBasicNode("BlockExpr", e, e->type()); return;
            case K::Return: PrintBasicNode("ReturnExpr", e, nullptr); return;
            case K::Call: PrintBasicNode("CallExpr", e, e->type()); return;
            case K::IntrinsicCall: PrintBasicNode("IntrinsicCallExpr", e, e->type()); return;
            case K::Cast: PrintBasicNode("CastExpr", e, e->type()); return;
            case K::MemberAccess: PrintBasicNode("MemberAccessExpr", e, e->type()); return;
        }

        PrintBasicNode(R"(<???>)", e, e->type());
    }

    /// Print a node.
    void operator()(const intc::Expr* e, std::string leading_text = "") {
        PrintHeader(e);

        /// Print the children of a node.
        using K = intc::Expr::Kind;
        switch (e->kind()) {
            case K::FuncDecl: {
                auto f = as<intc::FuncDecl>(e);
                if (auto block = cast<intc::BlockExpr>(const_cast<intc::FuncDecl*>(f)->body())) {
                    PrintChildren(block->children(), leading_text);
                } else {
                    intc::Expr* children[] = {const_cast<intc::FuncDecl*>(f)->body()};
                    PrintChildren(children, leading_text);
                }
            } break;

            case K::Binary: {
                auto b = as<intc::BinaryExpr>(e);
                intc::Expr* children[] = {b->lhs(), b->rhs()};
                PrintChildren(children, leading_text);
            } break;

            case K::NameRef: {
                auto n = as<intc::NameRefExpr>(e);
                if (n->target()) {
                    intc::Expr* children[] = {n->target()};
                    PrintChildren(children, leading_text);
                }
            } break;

            case K::VarDecl: {
                auto v = as<intc::VarDecl>(e);
                if (v->init()) {
                    intc::Expr* children[] = {v->init()};
                    PrintChildren(children, leading_text);
                }
            } break;

            case K::Unary: {
                auto u = as<intc::UnaryExpr>(e);
                intc::Expr* children[] = {u->operand()};
                PrintChildren(children, leading_text);
            } break;

            case K::Call: {
                auto c = as<intc::CallExpr>(e);
                std::vector<intc::Expr*> children{c->callee()};
                children.insert(children.end(), c->args().begin(), c->args().end());
                PrintChildren(children, leading_text);
            } break;

            case K::OverloadSet:
            case K::EvaluatedConstant:
            case K::While:
            case K::For:
            case K::Return:
            case K::StructDecl:
            case K::TypeAliasDecl:
            case K::IntegerLiteral:
            case K::StringLiteral:
            case K::CompoundLiteral:
            case K::If:
            case K::Block:
            case K::IntrinsicCall:
            case K::Cast:
            case K::MemberAccess:
                break;
        }
    }
};
} // namespace

auto intc::Type::string(bool use_colours) const -> std::string {
    lcc::utils::Colours C{use_colours};
    using enum lcc::utils::Colour;

    switch (kind()) {
        case Kind::Named: return as<NamedType>(this)->name();
        case Kind::Pointer:
            return fmt::format(
                "{}@{}{}",
                C(Red),
                C(Cyan),
                as<PointerType>(this)->element_type()->string(use_colours)
            );
        case Kind::Reference:
            return fmt::format(
                "{}&{}{}",
                C(Red),
                C(Cyan),
                as<ReferenceType>(this)->element_type()->string(use_colours)
            );

        case Kind::Integer: {
            auto i = as<IntegerType>(this);
            return fmt::format("{}{}{}", C(Cyan), i->is_signed() ? "i" : "u", i->bit_width());
        }

        case Kind::Struct: {
            auto decl = as<StructType>(this)->decl();
            return fmt::format(
                "{}struct {}{}",
                C(Red),
                C(Cyan),
                decl->name().empty() ? "<anonymous>" : decl->name()
            );
        }

        case Kind::Array: {
            auto arr = as<ArrayType>(this);
            if (auto sz = cast<IntegerLiteral>(arr->size())) {
                return fmt::format(
                    "{}{}[{}{}{}]",
                    arr->element_type()->string(use_colours),
                    C(Red),
                    C(Magenta),
                    sz->value(),
                    C(Red)
                );
            } else {
                return fmt::format(
                    "{}{}[{}?{}]",
                    arr->element_type()->string(use_colours),
                    C(Red),
                    C(Magenta),
                    C(Red)
                );
            }
        }

        case Kind::Builtin:
            switch (as<BuiltinType>(this)->builtin_kind()) {
                using K = BuiltinType::BuiltinKind;
                case K::Bool: return fmt::format("{}bool", C(Cyan));
                case K::Byte: return fmt::format("{}byte", C(Cyan));
                case K::Int: return fmt::format("{}int", C(Cyan));
                case K::Unknown: return fmt::format("{}<?>", C(Cyan));
                case K::Void: return fmt::format("{}void", C(Cyan));
                case K::OverloadSet: return fmt::format("{}<overload set>", C(Cyan));
            }
            LCC_UNREACHABLE();

        case Kind::FFIType:
            switch (as<FFIType>(this)->ffi_kind()) {
                using K = FFIType::FFIKind;
                case K::CChar: return fmt::format("__c_char", C(Cyan));
                case K::CSChar: return fmt::format("__c_schar", C(Cyan));
                case K::CUChar: return fmt::format("__c_uchar", C(Cyan));
                case K::CShort: return fmt::format("__c_short", C(Cyan));
                case K::CUShort: return fmt::format("__c_ushort", C(Cyan));
                case K::CInt: return fmt::format("__c_int", C(Cyan));
                case K::CUInt: return fmt::format("__c_uint", C(Cyan));
                case K::CLong: return fmt::format("__c_long", C(Cyan));
                case K::CULong: return fmt::format("__c_ulong", C(Cyan));
                case K::CLongLong: return fmt::format("__c_longlong", C(Cyan));
                case K::CULongLong: return fmt::format("__c_ulonglong", C(Cyan));
            }
            LCC_UNREACHABLE();

        case Kind::Function: {
            auto f = as<FuncType>(this);
            std::string out = fmt::format("{}{}(", f->return_type()->string(use_colours), C(Red));
            for (auto& arg : f->params()) {
                if (&arg != &f->params().front()) out += fmt::format("{}, ", C(Red));
                out += fmt::format("{}{}{}", C(Blue), arg.name, C(Red));
                if (not arg.name.empty()) out += " : ";
                else out += ":";
                out += arg.type->string(use_colours);
            }
            out += fmt::format("{})", C(Red));
            return out;
        }
    }

    LCC_UNREACHABLE();
}

void intc::Module::print() {
    ASTPrinter p{true};
    for (auto* node : as<BlockExpr>(top_level_function->body())->children())
        p(node);
}
