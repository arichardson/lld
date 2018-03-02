#pragma once

#include "../SymbolTable.h"
#include "../Symbols.h"
#include "../SyntheticSections.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/Support/Endian.h"

namespace lld {
namespace elf {

// See CheriBSD crt_init_globals()
template <llvm::support::endianness E> struct InMemoryCapRelocEntry {
  using CapRelocUint64 = llvm::support::detail::packed_endian_specific_integral<
      uint64_t, E, llvm::support::aligned>;
  InMemoryCapRelocEntry(uint64_t Loc, uint64_t Obj, uint64_t Off, uint64_t S,
                        uint64_t Perms)
      : capability_location(Loc), object(Obj), offset(Off), size(S),
        permissions(Perms) {}
  CapRelocUint64 capability_location;
  CapRelocUint64 object;
  CapRelocUint64 offset;
  CapRelocUint64 size;
  CapRelocUint64 permissions;
};

struct SymbolAndOffset {
  SymbolAndOffset(Symbol *S, int64_t O) : Sym(S), Offset(O) {}
  SymbolAndOffset(const SymbolAndOffset &) = default;
  SymbolAndOffset &operator=(const SymbolAndOffset &) = default;
  Symbol *Sym = nullptr;
  int64_t Offset = 0;

  // for __cap_relocs against local symbols clang emits section+offset instead
  // of the local symbol so that it still works even if the local symbol table
  // is stripped. This function tries to find the local symbol to a better match
  SymbolAndOffset findRealSymbol() const;

  template <typename ELFT> inline std::string verboseToString() const {
    return lld::verboseToString<ELFT>(Sym, Offset);
  }
};

struct CheriCapRelocLocation {
  InputSectionBase *Section;
  uint64_t Offset;
  bool NeedsDynReloc;
  bool operator==(const CheriCapRelocLocation &Other) const {
    return Section == Other.Section && Offset == Other.Offset;
  }
  template<typename ELFT>
  std::string toString() const;
};

struct CheriCapReloc {
  // We can't use a plain Symbol* here as capabilities to string constants
  // will be e.g. `.rodata.str + 0x90` -> need to store offset as well
  SymbolAndOffset Target;
  int64_t CapabilityOffset;
  bool NeedsDynReloc;
  bool operator==(const CheriCapReloc &Other) const {
    return Target.Sym == Other.Target.Sym &&
           Target.Offset == Other.Target.Offset &&
           CapabilityOffset == Other.CapabilityOffset &&
           NeedsDynReloc == Other.NeedsDynReloc;
  }
};

template <class ELFT> class CheriCapRelocsSection : public SyntheticSection {
public:
  CheriCapRelocsSection();
  static constexpr size_t RelocSize = 40;
  // Add a __cap_relocs section from in input object file
  void addSection(InputSectionBase *S);
  bool empty() const override { return RelocsMap.empty() && LegacyInputs.empty(); }
  size_t getSize() const override { return RelocsMap.size() * Entsize; }
  void finalizeContents() override;
  void writeTo(uint8_t *Buf) override;
  void addCapReloc(CheriCapRelocLocation Loc, const SymbolAndOffset &Target,
                   bool TargetNeedsDynReloc, int64_t CapabilityOffset,
                   Symbol *SourceSymbol = nullptr);

private:
  void processSection(InputSectionBase *S);
  bool addEntry(CheriCapRelocLocation Loc, CheriCapReloc Relocation) {
    auto it = RelocsMap.insert(std::make_pair(Loc, Relocation));
    // assert(it.first->second == Relocation);
    if (!(it.first->second == Relocation)) {
      error("Newly inserted relocation at " + Loc.toString<ELFT>() +
            " does not match existing one:\n>   Existing: " +
            it.first->second.Target.template verboseToString<ELFT>() +
            ", cap offset=" + Twine(it.first->second.CapabilityOffset) +
            ", dyn=" + Twine(it.first->second.NeedsDynReloc) +
            "\n>   New:     " + Relocation.Target.verboseToString<ELFT>() +
            ", cap offset=" + Twine(Relocation.CapabilityOffset) +
            ", dyn=" + Twine(Relocation.NeedsDynReloc));
    }
    return it.second;
  }
  // TODO: list of added dynamic relocations?

  llvm::MapVector<CheriCapRelocLocation, CheriCapReloc> RelocsMap;
  std::vector<InputSectionBase *> LegacyInputs;
  // If we have dynamic relocations we can't sort the __cap_relocs section
  // before writing it. TODO: actually we can but it will require refactoring
  bool ContainsDynamicRelocations = false;
  // If this is true reduce number of warnings for compat
  bool containsLegacyCapRelocs() const { return !LegacyInputs.empty(); }
};

class CheriCapTableSection : public SyntheticSection {
public:
  CheriCapTableSection();
  void addEntry(Symbol &Sym, bool NeedsSmallImm);
  uint32_t getIndex(const Symbol &Sym) const;
  bool empty() const override { return Entries.empty(); }
  void writeTo(uint8_t *Buf) override;
  template <class ELFT> void assignValuesAndAddCapTableSymbols();
  size_t getSize() const override {
    if (!Entries.empty() > 0)
      assert(Config->CapabilitySize > 0 &&
             "Cap table entries present but cap size unknown???");
    return Entries.size() * Config->CapabilitySize;
  }
private:
  struct CapTableIndex {
    // The index will be assigned once all symbols have been added
    // We do this so that we can order all symbols that need a small
    // immediate can be ordered before ones that are accessed using the
    // longer sequence of instructions
    // int64_t Index = -1;
    llvm::Optional<uint32_t> Index;
    bool NeedsSmallImm = false;
  };
  llvm::MapVector<Symbol *, CapTableIndex> Entries;
  bool ValuesAssigned = false;
};

template <typename ELFT, typename CallBack>
static void foreachGlobalSizesSymbol(InputSection *IS, CallBack &&CB) {
  assert(IS->Name == ".global_sizes");
  for (Symbol *B : IS->File->getSymbols()) {
    if (auto *D = dyn_cast<Defined>(B)) {
      if (D->Section != IS)
        continue;
      // skip the initial .global_sizes symbol (exists e.g. in
      // openpam_static_modules.o)
      if (D->isSection() && D->isLocal() && D->getName().empty())
        continue;
      StringRef Name = D->getName();
      if (!Name.startswith(".size.")) {
        error(".global_sizes symbol name is invalid: " +
              verboseToString<ELFT>(D));
        continue;
      }
      StringRef RealSymName = Name.drop_front(strlen(".size."));
      Symbol *Target = Symtab->find(RealSymName);
      CB(RealSymName, Target, D->Value);
    }
  }
}

} // namespace elf
} // namespace lld

namespace llvm {
template <> struct DenseMapInfo<lld::elf::CheriCapRelocLocation> {
  static inline lld::elf::CheriCapRelocLocation getEmptyKey() {
    return {nullptr, 0, false};
  }
  static inline lld::elf::CheriCapRelocLocation getTombstoneKey() {
    return {nullptr, std::numeric_limits<uint64_t>::max(), false};
  }
  static unsigned getHashValue(const lld::elf::CheriCapRelocLocation &Val) {
    auto Pair = std::make_pair(Val.Section, Val.Offset);
    return DenseMapInfo<decltype(Pair)>::getHashValue(Pair);
  }
  static bool isEqual(const lld::elf::CheriCapRelocLocation &LHS,
                      const lld::elf::CheriCapRelocLocation &RHS) {
    return LHS == RHS;
  }
};
} // namespace llvm
