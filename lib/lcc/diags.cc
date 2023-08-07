#include <lcc/context.hh>
#include <lcc/diags.hh>

#ifdef __linux__
#    include <execinfo.h>
#endif

/// ===========================================================================
///  Diagnostics.
/// ===========================================================================
namespace {
/// Get the colour of a diagnostic.
static constexpr auto Colour(lcc::Diag::Kind kind) {
    switch (kind) {
        case lcc::Diag::ICError: return fmt::fg(fmt::terminal_color::magenta) | fmt::emphasis::bold;
        case lcc::Diag::Warning: return fmt::fg(fmt::terminal_color::yellow) | fmt::emphasis::bold;
        case lcc::Diag::Note: return fmt::fg(fmt::terminal_color::green) | fmt::emphasis::bold;

        case lcc::Diag::FError:
        case lcc::Diag::Error:
            return fmt::fg(fmt::terminal_color::red) | fmt::emphasis::bold;

        default:
            return fmt::text_style{};
    }
}

/// Get the name of a diagnostic.
static constexpr std::string_view Name(lcc::Diag::Kind kind) {
    switch (kind) {
        case lcc::Diag::ICError: return "Internal Compiler Error";
        case lcc::Diag::FError: return "Fatal Error";
        case lcc::Diag::Error: return "Error";
        case lcc::Diag::Warning: return "Warning";
        case lcc::Diag::Note: return "Note";
        default: return "Diagnostic";
    }
}

#ifdef __linux__
/// Print the current stack trace.
void PrintBacktrace() {
    /// Get the backtrace.
    static void* trace[128];
    int n = backtrace(trace, 128);

    /// Convert to strings.
    std::vector<std::string> trace_strs;
    trace_strs.reserve(lcc::usz(n));
    for (int i = 0; i < n; i++) trace_strs.emplace_back(fmt::format("{:p}", trace[i]));

    /// Symboliser path.
    std::string sym = std::getenv("SYMBOLIZER_PATH") ?: "";
    if (sym.empty()) sym = "llvm-symbolizer";

    /// Use llvm-symbolizer to print the backtrace.
    auto cmd = fmt::format(
        "{} {} -e {} -s -p -C -i --color --output-style=GNU | awk '{{ print \"#\" NR, $0 }}'",
        sym,
        fmt::join(trace_strs, " "),
        lcc::fs::canonical("/proc/self/exe").native()
    );
    std::system(cmd.c_str());
}
#else
void PrintBacktrace() {
    /// TODO: Implement this for other platforms.
}
#endif
} // namespace

/// Abort due to assertion failure.
[[noreturn]] void lcc::detail::AssertFail(std::string&& msg) {
    Diag::ICE("{}", std::move(msg));
}

void lcc::Diag::HandleFatalErrors() {
    /// Abort on ICE.
    if (kind == ICError) {
        PrintBacktrace();
        std::exit(ICE_EXIT_CODE);
    }

    /// Exit on a fatal error.
    if (kind == FError) std::exit(FATAL_EXIT_CODE);
}

/// Print a diagnostic with no (valid) location info.
void lcc::Diag::PrintDiagWithoutLocation() {
    /// Print the message.
    fmt::print(stderr, Colour(kind), "{}: ", Name(kind));
    fmt::print(stderr, "{}\n", msg);
    HandleFatalErrors();
}

lcc::Diag::~Diag() {
    using fmt::fg;
    using enum fmt::emphasis;
    using enum fmt::terminal_color;

    /// If this diagnostic is suppressed, do nothing.
    if (kind == None) return;

    /// If the diagnostic is an error, set the error flag.
    if (kind == Error and ctx) ctx->set_error();

    /// If there is no context, then there is also no location info.
    if (not ctx) {
        PrintDiagWithoutLocation();
        return;
    }

    /// If the location is invalid, either because the specified file does not
    /// exists, its position is out of bounds or 0, or its length is 0, then we
    /// skip printing the location.
    const auto& fs = ctx->files();
    if (not Seekable()) {
        /// Even if the location is invalid, print the file name if we can.
        if (where.file_id < fs.size()) {
            const auto& file = *fs[where.file_id].get();
            fmt::print(stderr, bold, "{}: ", file.path().string());
        }

        /// Print the message.
        PrintDiagWithoutLocation();
        return;
    }

    /// If the location is valid, get the line, line number, and column number.
    const auto [line, col, line_start, line_end] = Seek();

    /// Split the line into everything before the range, the range itself,
    /// and everything after.
    std::string before(line_start, col);
    std::string range(line_start + col, where.len);
    std::string after(line_start + col + where.len, line_end);

    /// Replace tabs with spaces. We need to do this *after* splitting
    /// because this invalidates the offsets.
    utils::ReplaceAll(before, "\t", "    ");
    utils::ReplaceAll(range, "\t", "    ");
    utils::ReplaceAll(after, "\t", "    ");

    /// Print the file name, line number, and column number.
    const auto& file = *fs[where.file_id].get();
    fmt::print(stderr, bold, "{}:{}:{}: ", file.path().string(), line, col);

    /// Print the diagnostic name and message.
    fmt::print(stderr, Colour(kind), "{}: ", Name(kind));
    fmt::print(stderr, "{}\n", msg);

    /// Print the line up to the start of the location, the range in the right
    /// colour, and the rest of the line.
    fmt::print(stderr, " {} | {}", line, before);
    fmt::print(stderr, Colour(kind), "{}", range);
    fmt::print(stderr, "{}\n", after);

    /// Determine the number of digits in the line number.
    const auto digits = utils::NumberWidth(line);

    /// Underline the range. For that, we first pad the line based on the number
    /// of digits in the line number and append more spaces to line us up with
    /// the range.
    for (usz i = 0; i < digits + before.size() + sizeof("  | ") - 1; i++)
        fmt::print(stderr, " ");

    /// Finally, underline the range.
    for (usz i = 0; i < range.size(); i++) fmt::print(stderr, Colour(kind), "~");
    fmt::print(stderr, "\n");

    /// Handle fatal errors.
    HandleFatalErrors();
}

bool lcc::Diag::Seekable() const {
    auto& files = ctx->files();
    if (where.file_id >= files.size()) return false;
    const auto* f = files[where.file_id].get();
    return where.pos + where.len <= f->size() and where.len;
}

/// Seek to a source location. The location must be valid.
auto lcc::Diag::Seek() const -> LocInfo {
    LocInfo info{};

    /// Get the file that the location is in.
    auto& files = ctx->files();
    const auto* f = files.at(where.file_id).get();

    /// Seek back to the start of the line.
    const char* const data = f->data();
    info.line_start = data + where.pos;
    while (info.line_start > data and *info.line_start != '\n') info.line_start--;
    if (*info.line_start == '\n') info.line_start++;

    /// Seek forward to the end of the line.
    const char* const end = data + f->size();
    info.line_end = data + where.pos + where.len;
    while (info.line_end < end and *info.line_end != '\n') info.line_end++;

    /// Determine the line and column number.
    info.line = 1;
    for (const char* d = data; d < data + where.pos; d++) {
        if (*d == '\n') {
            info.line++;
            info.col = 0;
        } else {
            info.col++;
        }
    }

    /// Done!
    return info;
}

/// TODO: Lexer should create map that counts where in a file the lines start so
/// we can do binary search on that instead of iterating over the entire file.
auto lcc::Diag::SeekLineColumn() const -> LocInfoShort {
    LocInfoShort info{};

    /// Get the file that the location is in.
    auto& files = ctx->files();
    const auto* f = files.at(where.file_id).get();

    /// Seek back to the start of the line.
    const char* const data = f->data();

    /// Determine the line and column number.
    info.line = 1;
    for (const char* d = data; d < data + where.pos; d++) {
        if (*d == '\n') {
            info.line++;
            info.col = 0;
        } else {
            info.col++;
        }
    }

    /// Done!
    return info;
}
