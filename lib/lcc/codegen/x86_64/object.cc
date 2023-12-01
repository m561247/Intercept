#include <lcc/codegen/generic_object.hh>
#include <lcc/codegen/x86_64/object.hh>
#include <lcc/context.hh>

namespace lcc {
namespace x86_64 {

GenericObject emit_mcode_gobj(Module* module, const MachineDescription& desc, std::vector<MFunction>& mir) {
    GenericObject out{};

    Section text_{".text", {}, 0, 0, false};
    Section data_{".data", {}, 0, 0, false};
    Section bss_{".bss", {}, 0, 0, true};
    out.sections.push_back(text_);
    out.sections.push_back(data_);
    out.sections.push_back(bss_);

    Section& text = out.section(".text");
    Section& data = out.section(".data");
    Section& bss = out.section(".bss");

    for (auto* var : module->vars())
        out.symbol_from_global(var);

    for (auto& func : mir) {
        const bool exported = func.linkage() == Linkage::Exported || func.linkage() == Linkage::Reexported;
        const bool imported = func.linkage() == Linkage::Imported || func.linkage() == Linkage::Reexported;

        if (imported) {
            Symbol sym{};
            sym.kind = Symbol::Kind::EXTERNAL;
            sym.name = func.name();
            // FIXME: Is section name or byte offset needed?
            out.symbols.push_back(sym);
        } else {
            Symbol sym{};
            sym.kind = Symbol::Kind::FUNCTION;
            sym.name = func.name();
            sym.section_name = text.name;
            sym.byte_offset = text.contents.size();
            out.symbols.push_back(sym);
        }

        // TODO: Assemble function into machine code.

    }

    LCC_TODO("Actually assemble into machine code, populate symbols, etc");

    return {};
}

} // namespace x86_64
} // namespace lcc
