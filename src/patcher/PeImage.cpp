// SPDX-License-Identifier: GPL-3.0-or-later

#include "patcher/PeImage.hpp"

#include <windows.h>

#include <cstring>

namespace wxl::patcher
{
    namespace
    {
        constexpr size_t   kSectionNameLength = IMAGE_SIZEOF_SHORT_NAME;
        constexpr uint32_t kHintLength        = 2;

        uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

        IMAGE_NT_HEADERS32* Nt(std::vector<uint8_t>& bytes)
        {
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
            return reinterpret_cast<IMAGE_NT_HEADERS32*>(bytes.data() + dos->e_lfanew);
        }

        const IMAGE_NT_HEADERS32* Nt(const std::vector<uint8_t>& bytes)
        {
            return Nt(const_cast<std::vector<uint8_t>&>(bytes));
        }

        /// The file offset of a relative address, 0 when no section holds it.
        uint32_t RvaToOffset(const IMAGE_NT_HEADERS32* nt, uint32_t rva)
        {
            const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
            for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            {
                const IMAGE_SECTION_HEADER& s    = sections[i];
                const uint32_t              span = s.Misc.VirtualSize > s.SizeOfRawData ? s.Misc.VirtualSize : s.SizeOfRawData;
                if (rva >= s.VirtualAddress && rva < s.VirtualAddress + span) return rva - s.VirtualAddress + s.PointerToRawData;
            }
            return 0;
        }

        /// Where each part of the new import section goes: the descriptors (the old ones, ours, the
        /// terminator), the name and address tables, the imported name, the DLL name.
        struct ImportLayout
        {
            uint32_t descriptors, names, addresses, byName, dll, size;

            ImportLayout(uint32_t oldCount, const char* dll, const char* func)
            {
                descriptors = 0;
                names       = descriptors + (oldCount + 2) * sizeof(IMAGE_IMPORT_DESCRIPTOR);
                addresses   = names + 2 * sizeof(uint32_t);
                byName      = addresses + 2 * sizeof(uint32_t);
                this->dll   = byName + AlignUp(kHintLength + uint32_t(std::strlen(func)) + 1, 2);
                size        = this->dll + uint32_t(std::strlen(dll)) + 1;
            }
        };
    }

    bool PeImage::Valid() const
    {
        if (m_bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m_bytes.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        if (static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > m_bytes.size()) return false;
        const IMAGE_NT_HEADERS32* nt = Nt(m_bytes);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    }

    bool PeImage::HasSection(const char* name) const
    {
        const IMAGE_NT_HEADERS32*   nt       = Nt(m_bytes);
        const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
        const size_t                length   = strnlen(name, kSectionNameLength);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (std::memcmp(sections[i].Name, name, length) == 0 &&
                (length == kSectionNameLength || sections[i].Name[length] == 0))
                return true;
        return false;
    }

    void PeImage::SetLargeAddressAware() { Nt(m_bytes)->FileHeader.Characteristics |= IMAGE_FILE_LARGE_ADDRESS_AWARE; }

    bool PeImage::AddImport(const char* dll, const char* func, const char* tagSection)
    {
        if (HasSection(tagSection)) return true;

        IMAGE_NT_HEADERS32*      nt       = Nt(m_bytes);
        IMAGE_FILE_HEADER&       file     = nt->FileHeader;
        IMAGE_OPTIONAL_HEADER32& optional = nt->OptionalHeader;
        IMAGE_SECTION_HEADER*    sections = IMAGE_FIRST_SECTION(nt);

        const uint32_t importOffset = RvaToOffset(nt, optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        if (importOffset == 0 || importOffset + sizeof(IMAGE_IMPORT_DESCRIPTOR) > m_bytes.size()) return false;
        auto*    imports  = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(m_bytes.data() + importOffset);
        uint32_t oldCount = 0;
        while (importOffset + (oldCount + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR) <= m_bytes.size() &&
               (imports[oldCount].Name != 0 || imports[oldCount].FirstThunk != 0))
            ++oldCount;

        const ImportLayout          at(oldCount, dll, func);
        const IMAGE_SECTION_HEADER& last       = sections[file.NumberOfSections - 1];
        const uint32_t              sectionRva = AlignUp(last.VirtualAddress + last.Misc.VirtualSize, optional.SectionAlignment);
        const uint32_t              sectionRaw = AlignUp(uint32_t(m_bytes.size()), optional.FileAlignment);

        std::vector<uint8_t> blob(at.size, 0);
        auto* descriptors = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(blob.data() + at.descriptors);
        std::memcpy(descriptors, imports, oldCount * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        descriptors[oldCount].OriginalFirstThunk = sectionRva + at.names;
        descriptors[oldCount].Name               = sectionRva + at.dll;
        descriptors[oldCount].FirstThunk         = sectionRva + at.addresses;
        *reinterpret_cast<uint32_t*>(blob.data() + at.names)     = sectionRva + at.byName;
        *reinterpret_cast<uint32_t*>(blob.data() + at.addresses) = sectionRva + at.byName;
        strcpy_s(reinterpret_cast<char*>(blob.data() + at.byName + kHintLength), at.size - (at.byName + kHintLength), func);
        strcpy_s(reinterpret_cast<char*>(blob.data() + at.dll), at.size - at.dll, dll);

        // The new section header must fit in the headers' padding.
        IMAGE_SECTION_HEADER& added = sections[file.NumberOfSections];
        if (reinterpret_cast<uint8_t*>(&added + 1) - m_bytes.data() > static_cast<ptrdiff_t>(optional.SizeOfHeaders))
            return false;
        std::memset(&added, 0, sizeof added);
        std::memcpy(added.Name, tagSection, strnlen(tagSection, kSectionNameLength));
        added.Misc.VirtualSize = at.size;
        added.VirtualAddress   = sectionRva;
        added.SizeOfRawData    = AlignUp(at.size, optional.FileAlignment);
        added.PointerToRawData = sectionRaw;
        added.Characteristics  = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]       = { sectionRva + at.descriptors, at.names };
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT] = { 0, 0 };
        optional.SizeOfImage = AlignUp(sectionRva + at.size, optional.SectionAlignment);
        file.NumberOfSections += 1;

        // Growing the buffer moves it: every pointer above is dead after this.
        const uint32_t rawSize = added.SizeOfRawData;
        m_bytes.resize(sectionRaw, 0);
        m_bytes.insert(m_bytes.end(), blob.begin(), blob.end());
        m_bytes.resize(sectionRaw + rawSize, 0);
        return true;
    }
}
