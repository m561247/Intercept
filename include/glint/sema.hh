#ifndef GLINT_SEMA_HH
#define GLINT_SEMA_HH

#include <lcc/utils.hh>
#include <lcc/utils/aint.hh>

#include <glint/ast.hh>

#include <string>
#include <utility>
#include <vector>

namespace lcc::glint {
class Sema {
    Context* context;
    Module& mod;

    /// The function we’re currently analysing.
    FuncDecl* curr_func;

    /// Whether to use colours in diagnostics.
    bool _use_colours;

    Sema(Context* ctx, Module& module, bool use_colours)
        : context(ctx),
          mod(module),
          curr_func(mod.top_level_function()),
          _use_colours(use_colours) {}
public:
    /// Perform semantic analysis on the given module.
    ///
    /// To check for errors, check the has_error() flag
    /// of the context.
    ///
    /// \param ctx The context that owns the module.
    /// \param m The module to analyse.
    /// \param use_colours Whether to use colours in diagnostics.
    static void Analyse(Context* ctx, Module& m, bool use_colours = false);

private:
    /// \see Error()
    template <typename Ty>
    struct format_type {
        using type = Ty;
    };

    template <typename Ty>
    requires requires (Ty ty) { cast<Type>(ty); }
    struct format_type<Ty> {
        using type = std::string;
    };

    template <typename Ty>
    using format_type_t = typename format_type<Ty>::type;

    [[nodiscard]]
    auto Analyse(Type** type) -> bool;
    [[nodiscard]]
    auto Analyse(Expr** expr, Type* expected_type = nullptr) -> bool;
    // expr_ptr points to binary expression `b`
    void AnalyseBinary(Expr** expr_ptr, BinaryExpr* b);
    // expr_ptr points to call expression `expr`
    void AnalyseCall(Expr** expr_ptr, CallExpr* expr);
    void AnalyseCast(CastExpr* expr);
    void AnalyseFunctionBody(FuncDecl* decl);
    void AnalyseFunctionSignature(FuncDecl* decl);
    // expr_ptr points to intrinsic call expression `expr`
    void AnalyseIntrinsicCall(Expr** expr_ptr, IntrinsicCallExpr* expr);
    void AnalyseModule();
    void AnalyseNameRef(NameRefExpr* expr);
    // expr_ptr points to unary expression `u`
    void AnalyseUnary(Expr** expr_ptr, UnaryExpr* u);

    /// Rewrite given expression pointer to:
    /// BINARY :=
    /// |-- lhs
    /// `-- BINARY op
    ///     |-- lhs
    ///     `-- rhs
    void RewriteToBinaryOpThenAssign(
        Expr** expr_ptr,
        TokenKind op,
        Expr* lhs,
        Expr* rhs,
        Location location = {}
    );
    void RewriteToBinaryOpThenAssign(
        Expr** expr_ptr,
        TokenKind op,
        BinaryExpr* b
    ) {
        RewriteToBinaryOpThenAssign(
            expr_ptr,
            op,
            b->lhs(),
            b->rhs(),
            b->location()
        );
    }
    /// Analyse an expression and discard it.
    /// \see Discard.
    [[nodiscard]]
    auto AnalyseAndDiscard(Expr** expr) -> bool;

    /// Attempt to convert an expression to a given type.
    ///
    /// This may replace the expression with a cast Note that the
    /// expression to be converted must be marked as either done
    /// or errored by sema. If marked as errored, this always
    /// returns true and does nothing so.
    ///
    /// \param expr A pointer to the expression to convert.
    /// \param type The type to convert to.
    /// \return Whether the conversion succeeded.
    /// \see TryConvert().
    [[nodiscard]]
    auto Convert(Expr** expr, Type* type) -> bool;

    /// Do not call this directly. Call \c Convert() or \c TryConvert() instead.
    template <bool PerformConversion>
    [[nodiscard]]
    auto ConvertImpl(Expr** expr_ptr, Type* to) -> int;

    /// Like Convert(), but issue an error if the conversion fails.
    ///
    /// Prefer using Convert() and issuing an error manually as that is usually
    /// more informative. Use this only when there really are no semantics to the
    /// conversion other than ‘type X must be convertible to type Y’.
    void ConvertOrError(Expr** expr, Type* to);

    /// Like Convert(), but tries converting a to b and b to a.
    ///
    /// \param a The first expression.
    /// \param b The second expression.
    /// \return Whether the conversion succeeded.
    /// \see ConvertToCommonType()
    [[nodiscard]]
    auto ConvertToCommonType(Expr** a, Expr** b) -> bool;

    /// Convert a type to a type that is legal in a declaration.
    [[nodiscard]]
    auto DeclTypeDecay(Type* type) -> Type*;

    /// Apply deproceduring conversion. This may insert a call.
    /// \return Whether a call was inserted.
    [[nodiscard]]
    auto Deproceduring(Expr** expr) -> bool;

    /// Mark an expression as discarded. Depending on the expression, this
    /// will do several things, such as deproceduring, checking unused results
    /// and so on.
    ///
    /// This should be called on any expression that occurs in a context where
    /// its value is not used, irrespective of what the type of the expression is.
    void Discard(Expr** expr);

    /// Wrapper that stringifies any types that are passed in and passes
    /// everything to \c Diag::Error.
    template <typename... Args>
    auto Error(Location loc, fmt::format_string<format_type_t<Args>...> fmt, Args&&... args) {
        return Diag::Error(context, loc, fmt, Format<Args>(std::forward<Args>(args))...);
    }

    /// Wrapper that stringifies any types that are passed in and passes
    /// everything to \c Diag::Warning.
    template <typename... Args>
    auto Warning(Location loc, fmt::format_string<format_type_t<Args>...> fmt, Args&&... args) {
        return Diag::Warning(context, loc, fmt, Format<Args>(std::forward<Args>(args))...);
    }

    /// Wrapper that stringifies any types that are passed in and passes
    /// everything to \c Diag::Note.
    template <typename... Args>
    auto Note(Location loc, fmt::format_string<format_type_t<Args>...> fmt, Args&&... args) {
        return Diag::Note(context, loc, fmt, Format<Args>(std::forward<Args>(args))...);
    }

    /// Evaluate a constant expression and ensure it is an integer.
    [[nodiscard]]
    auto EvaluateAsInt(Expr* expr, Type* int_type, aint& out) -> bool;

    /// Format a type.
    template <typename Ty>
    requires requires (Ty ty) { cast<Type>(ty); }
    auto Format(Ty ty) -> std::string { return ty->string(_use_colours); }

    /// Formatting anything else just passes it through unchanged.
    template <typename Ty>
    requires (not requires (Ty ty) { cast<Type>(ty); })
    auto Format(Ty&& t) -> decltype(std::forward<Ty>(t)) {
        return std::forward<Ty>(t);
    }

    /// Check if an expression has side effects.
    [[nodiscard]]
    static auto HasSideEffects(Expr* expr) -> bool;

    /// Dereference an expression, potentially yielding an lvalue.
    ///
    /// This differs from LValueToRValue conversion in that it
    ///
    ///     1. strips pointers too, and
    ///     2. produces an lvalue if possible.
    ///
    /// \return Whether the result is an lvalue.
    [[nodiscard]]
    auto ImplicitDereference(Expr** expr) -> bool;

    /// De-reference an expression, potentially yielding an lvalue.
    ///
    /// Does not do anything to pointers.
    ///
    /// \return Whether the result is an lvalue.
    [[nodiscard]]
    auto ImplicitDe_Reference(Expr** expr) -> bool;

    /// Insert an implicit cast of an expression to a type.
    ///
    /// This creates a new cast expression and replaces the expression
    /// pointed to by \c expr_ptr with a cast to \c to. The location of
    /// the cast expression is set to the location of the old expression.
    void InsertImplicitCast(Expr** expr_ptr, Type* ty);

    /// If the type of an expression is a pointer type—not a reference
    /// type—convert the expression to \c integer instead by inserting
    /// a cast expression.
    ///
    /// Otherwise, this is a no-op.
    void InsertPointerToIntegerCast(Expr** operand);

    /// Convert lvalues to rvalues and leave rvalues unchanged. Also
    /// convert references to rvalues of their referenced type.
    ///
    /// This may insert a cast expression.
    ///
    /// \param expr A pointer to the expression to convert.
    /// \param strip_ref Whether to also convert a reference to
    ///     the referenced type, if present.
    /// \return The type of the rvalue.
    void LValueToRValue(Expr** expr, bool strip_ref = true);

    /// Create a (type-checked) pointer to a type.
    auto Ptr(Type* type) -> PointerType*;

    /// Create a (type-checked) reference to a type.
    auto Ref(Type* type) -> ReferenceType*;

    /// Attempt to convert an expression to a given type.
    ///
    /// This is similar to \c Convert(), except that it does not perform
    /// any conversion and that it doesn’t issue a diagnostic on failure.
    ///
    /// Furthermore, this returns a score that may be used for overload
    /// resolution. The score indicates how ‘bad’ the conversion is, i.e. how
    /// badly the overload containing it matches the type of the expression.
    ///
    /// Note that, unlike \c Convert(), this function returns does not succeed
    /// if the expression is marked as errored.
    ///
    /// \param expr A pointer to the expression to convert.
    /// \param type The type to convert to.
    /// \return -2 if the expression is marked as errored.
    /// \return -1 if the conversion fails or is impossible.
    /// \return 0 if the conversion is (logically) a no-op.
    /// \return A number greater than 0 that indicates how ‘bad’ the conversion is.
    /// \see Convert().
    [[nodiscard]] int TryConvert(Expr** expr, Type* type);

    /// Wrap an expression with a cast.
    ///
    /// This replaces an expression with a cast expression to
    /// the designated type. The location of the cast is set
    /// to the location of the expression.
    void WrapWithCast(Expr** expr, Type* type, CastKind kind);

    auto try_get_metadata_blob_from_gmeta(
        const Module::Ref& import,
        const std::string& include_dir,
        std::vector<std::string>& paths_tried
    ) -> bool;
    auto try_get_metadata_blob_from_object(
        const Module::Ref& import,
        const std::string& include_dir,
        std::vector<std::string>& paths_tried
    ) -> bool;
    auto try_get_metadata_blob_from_assembly(
        const Module::Ref& import,
        const std::string& include_dir,
        std::vector<std::string>& paths_tried
    ) -> bool;
};
} // namespace lcc::glint

#endif // GLINT_SEMA_HH
