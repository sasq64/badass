#include "machine.h"
#include "cart.h"
#include "defines.h"
#include "emulator.h"

#include <coreutils/algorithm.h>
#include <coreutils/file.h>
#include <coreutils/log.h>

#include <algorithm>
#include <array>
#include <map>
#include <vector>

inline void Check(bool v, std::string const& txt)
{
    if (!v) throw machine_error(txt);
}

Machine::Machine()
{
    machine = std::make_unique<sixfive::Machine<>>();
    addSection({"default", 0});
    setSection("default");
    machine->setBreakFunction(breakFunction, this);
}

void Machine::breakFunction(int what, void* data)
{
    auto* thiz = static_cast<Machine*>(data);

    auto it = thiz->break_functions.find(what);
    if (it != thiz->break_functions.end()) {
        it->second(what);
    } else {
        auto [a, x, y, sr, sp, pc] = thiz->getRegs();
        fmt::print("**Error: Unhandled BRK #${:x}\n", what);
        fmt::print("A:{:x} X:{:x} Y:{:x} PC={:04x}\n", a, x, y, pc);
        fmt::print("{:04x}: {}\n", pc, thiz->disassemble(&pc));

        exit(0);
    }
}

Machine::~Machine() = default;

void Machine::setBreakFunction(uint8_t what,
                               std::function<void(uint8_t)> const& fn)
{
    break_functions[what] = fn;
}

Section& Machine::addSection(Section const& s)
{
    auto [name, in, children, start, pc, size, flags, data, valid] = s;

    auto it =
        std::find_if(sections.begin(), sections.end(),
                     [name = name](auto const& as) { return as.name == name; });

    Section& section =
        it == sections.end() ? sections.emplace_back(name, -1) : *it;

    Check(section.data.empty(), "Section already populated");

    section.flags = flags;
    section.pc = pc;
    if (size != -1) {
        section.size = size;
        section.flags |= SectionFlags::FixedSize;
    }

    if (start != -1) {
        section.start = start;
        section.flags |= SectionFlags::FixedStart;
    }

    if (!in.empty()) {
        auto& parent = getSection(in);
        LOGI("Parent %s at %x/%x", parent.name, parent.start, parent.pc);
        Check(parent.data.empty(), "Parent section must contain no data");

        if (section.parent.empty()) {
            section.parent = in;
            parent.children.push_back(section.name);
        }

        if ((parent.flags & SectionFlags::ReadOnly) != 0) {
            section.flags |= SectionFlags::ReadOnly;
        }

        if (section.start == -1) {
            LOGI("Setting start to %x", parent.pc);
            section.start = parent.pc;
        }
    }

    if (section.pc == -1) {
        section.pc = section.start;
    }

    Check(section.start != -1, "Section must have start");

    return section;
}

void Machine::removeSection(std::string const& name)
{
    auto it = std::find_if(sections.begin(), sections.end(),
                           [&](auto const& s) { return s.name == name; });
    if (it != sections.end()) {
        sections.erase(it);
    }
}

// Layout section 's', exactly at address if Floating, otherwise
// it must at least be placed after address
// Return section end
int32_t Machine::layoutSection(int32_t address, Section& s)
{
    if (!s.valid) {
        LOGI("Skipping invalid section %s", s.name);
        return address;
    }

    LOGD("Layout %s", s.name);
    if ((s.flags & FixedStart) == 0) {
        if (s.start != address) {
            LOGD("%s: %x differs from %x", s.name, s.start, address);
            layoutOk = false;
        }
        s.start = address;
    }

    Check(s.start >= address,
          fmt::format("Section {} starts at {:x} which is before {:x}", s.name,
                      s.start, address));

    if (!s.data.empty()) {
        Check(s.children.empty(), "Data section may not have children");
        // Leaf / data section
        return s.start + static_cast<int32_t>(s.data.size());
    }

    if (!s.children.empty()) {
        // Lay out children
        for (auto const& child : s.children) {
            address = layoutSection(address, getSection(child));
        }
    }
    // Unless fixed size, update size to total of its children
    if ((s.flags & FixedSize) == 0) {
        s.size = address - s.start;
    }
    if (address - s.start > s.size) {
        throw machine_error(fmt::format("Section {} is too large", s.name));
    }
    return s.start + s.size;
}

bool Machine::layoutSections()
{
    layoutOk = true;
    // Lay out all root sections
    for (auto& s : sections) {
        if (s.parent.empty()) {
            // LOGI("Root %s at %x", s.name, s.start);
            auto start = s.start;
            layoutSection(start, s);
        }
    }
    return layoutOk;
}

Error Machine::checkOverlap()
{
    for (auto& a : sections) {
        if (!a.data.empty()) {
            for (auto const& b : sections) {
                if (&a != &b && !b.data.empty()) {
                    auto as = a.start;
                    auto ae = as + static_cast<int32_t>(a.data.size());
                    auto bs = b.start;
                    auto be = bs + static_cast<int32_t>(b.data.size());
                    if (as >= bs && as < be) {
                        return {2, 0,
                                fmt::format("Section {} overlaps {}", a.name,
                                            b.name)};
                    }
                    if (bs >= as && bs < ae) {
                        return {2, 0,
                                fmt::format("Section {} overlaps {}", b.name,
                                            a.name)};
                    }
                }
            }
        }
    }
    return {};
}

Section& Machine::getSection(std::string const& name)
{
    auto it = std::find_if(sections.begin(), sections.end(),
                           [&](auto const& s) { return s.name == name; });
    if (it == sections.end()) {
        throw machine_error(fmt::format("Unknown section {}", name));
    }
    return *it;
}
Section& Machine::getCurrentSection()
{
    return *currentSection;
}
void Machine::pushSection(std::string const& name)
{
    savedSections.push_back(currentSection);
    setSection(name);
}

void Machine::dropSection()
{
    savedSections.pop_back();
}

void Machine::popSection()
{
    currentSection = savedSections.back();
    savedSections.pop_back();
}

void Machine::setSection(std::string const& name)
{
    currentSection = &getSection(name);
    currentSection->valid = true;
}

void Machine::clear()
{
    if (fp != nullptr) {
        rewind(fp);
    }
    for (auto& s : sections) {
        s.data.clear();
        s.pc = s.start;
        s.valid = false;
    }
    setSection("default");
}

uint32_t Machine::getPC() const
{
    if (currentSection == nullptr) {
        return 0;
    }
    return currentSection->pc;
}

// clang-format off
constexpr static std::array modeTemplate = {
        "",
        "",
        "A",

        "#$%02x",
        "$%04x",

        "$%02x",
        "$%02x,x",
        "$%02x,y",
        "($%02x,x)",
        "($%02x),y",
        "($%02x)",

        "($%04x)",
        "$%04x",
        "$%04x,x",
        "$%04x,y",
};
// clang-format on

void writeChip(utils::File const& outFile, int bank, int startAddress,
               std::vector<uint8_t> const& data)
{
    outFile.writeString("CHIP");
    outFile.writeBE<uint32_t>(data.size() + 0x10);
    outFile.writeBE<uint16_t>(ChipType::Rom);
    outFile.writeBE<uint16_t>(bank);
    outFile.writeBE<uint16_t>(startAddress);
    outFile.writeBE<uint16_t>(data.size());
    outFile.write(data);
}

struct Chip
{
    int bank;
    uint16_t start;
    std::vector<uint8_t> data = std::vector<uint8_t>(0x2000);
};

void Machine::writeCrt(utils::File const& outFile)
{
    std::map<uint32_t, Chip> chips;
    bool banked = false;
    uint8_t exrom = 0;
    uint8_t game = 1;

    // 8000 -> bfff
    // e000 -> ffff
    for (auto const& section : sections) {
        if (section.data.empty()) {
            continue;
        }
        LOGI("Start %x", section.start);
        auto bank = section.start >> 16;
        auto start = section.start & 0xffff;
        auto end = start + section.data.size();
        // bank : 01a000,01c000,01e000
        if (start < 0x8000 || end > 0xc000) {
            throw machine_error("Can't write crt");
        }

        if (end >= 0xa000) {
            game = 0;
        }

        auto offset = start - 0x8000;

        if (bank > 0) {
            banked = true;
        }

        auto& chip = chips[bank << 16 | 0x8000];
        LOGI("Putting section %s in %x at offset %x", section.name, bank,
             offset);
        if (static_cast<int32_t>(chip.data.size()) <= offset) {
            chip.data.resize(0x4000);
        }
        memcpy(&chip.data[offset], section.data.data(), section.data.size());
    }

    const uint32_t headerLength = 0x40;
    const uint16_t version = 0x0100;
    const uint16_t hardware =
        banked ? CartType::EasyFlash : CartType::Normalcartridge;

    std::string name = "TEST";
    std::array<char, 32> label{};
    std::copy(name.begin(), name.end(), label.data());

    outFile.writeString("C64 CARTRIDGE   ");
    outFile.writeBE(headerLength);
    outFile.writeBE(version);
    outFile.writeBE(hardware);
    outFile.write<uint8_t>(exrom);
    outFile.write<uint8_t>(game);
    outFile.write(std::vector<uint8_t>{0, 0, 0, 0, 0, 0});
    outFile.write(label);

    for (auto const& e : chips) {
        auto bank = e.first >> 16;
        auto start = e.first & 0xffff;
        LOGD("Writing %x/%x", bank, start);
        writeChip(outFile, bank, start, e.second.data);
    }
}

void Machine::write(std::string const& name, OutFmt fmt)
{
    auto non_empty = utils::filter_to(
        sections, [](auto const& s) { return !s.data.empty(); });

    if (non_empty.empty()) {
        puts("**Warning: No sections");
        return;
    }

    LOGD("%d data sections", non_empty.size());

    std::sort(non_empty.begin(), non_empty.end(),
              [](auto const& a, auto const& b) { return a.start < b.start; });

    int32_t last_end = -1;
    utils::File outFile{name, utils::File::Mode::Write};

    auto start = non_empty.front().start;
    auto end = non_empty.back().start +
               static_cast<int32_t>(non_empty.back().data.size());

    if (end <= start) {
        puts("**Warning: No code generated");
        return;
    }

    if (fmt == OutFmt::Crt) {
        writeCrt(outFile);
        return;
    }

    if (fmt == OutFmt::Prg) {
        outFile.write<uint8_t>(start & 0xff);
        outFile.write<uint8_t>(start >> 8);
    }

    // bool bankFile = false;

    for (auto const& section : non_empty) {

        if (section.start < last_end) {
            throw machine_error(
                fmt::format("Section {} overlaps previous", section.name));
        }

        if (section.data.empty()) {
            continue;
        }

        if ((section.flags & WriteToDisk) != 0) {
            utils::File of{section.name, utils::File::Mode::Write};
            of.write<uint8_t>(section.start & 0xff);
            of.write<uint8_t>(section.start >> 8);
            of.write(section.data);
            of.close();
            continue;
        }

        if ((section.flags & NoStorage) != 0) {
            continue;
        }

        auto offset = section.start;
        auto adr = section.start & 0xffff;
        auto hi_adr = section.start >> 16;

        if (hi_adr > 0) {
            offset = section.start & 0xffff;
            if (adr >= 0xa000 && adr + section.data.size() <= 0xc000) {
                offset = hi_adr * 8192 + adr;
            } else {
                throw machine_error("Illegal address");
            }
        }

        if (last_end >= 0) {
            // LOGI("Padding %d bytes", offset - last_end);
            while (last_end < offset) {
                outFile.write<uint8_t>(0);
                last_end++;
            }
        }

        last_end = static_cast<uint32_t>(offset + section.data.size());

        // LOGI("Writing %d bytes", section.data.size());
        outFile.write(section.data);
    }
}

uint32_t Machine::run(uint16_t pc)
{
    for (auto const& section : sections) {
        if (section.start < 0x10000 && !section.data.empty()) {
            LOGI("Writing '%s' to %x", section.name, section.start);
            machine->writeRam(section.start, section.data.data(),
                              section.data.size());
        }
    }
    fmt::print("Running code at ${:x}\n", pc);
    machine->setPC(pc);
    return machine->run();
}

void Machine::setOutput(FILE* f)
{
    fp = f;
}

uint32_t Machine::writeByte(uint8_t w)
{
    currentSection->data.push_back(w);
    currentSection->pc++;
    return currentSection->pc;
}

uint32_t Machine::writeChar(uint8_t w)
{
    if (fp != nullptr) {
        if (!inData) {
            fprintf(fp, "%04x : \"", currentSection->pc);
        }
        inData = true;
        fputc(w, fp);
    }
    currentSection->data.push_back(w);
    currentSection->pc++;
    return currentSection->pc;
}

std::string Machine::disassemble(uint32_t* pc)
{
    auto code = machine->readMem(*pc);
    pc++;
    auto const& instructions = sixfive::Machine<>::getInstructions();
    sixfive::Machine<>::Opcode opcode{};

    std::string name = "???";
    for (auto const& i : instructions) {
        for (auto const& o : i.opcodes) {
            if (o.code == code) {
                name = i.name;
                opcode = o;
                break;
            }
        }
    }
    auto sz = opSize(opcode.mode);
    int32_t arg = 0;
    if (sz == 3) {
        arg = machine->readMem(pc[0]) | (machine->readMem(pc[1]) << 8);
        pc += 2;
    } else if (sz == 2) {
        arg = machine->readMem(*pc++);
    }

    if (opcode.mode == sixfive::AddressingMode::REL) {
        arg = (static_cast<int8_t>(arg)) + *pc;
    }

    std::array<char, 16> argstr; // NOLINT
    sprintf(argstr.data(), modeTemplate.at(opcode.mode), arg);

    return fmt::format("{} {}", name, argstr.data());
}

AsmResult Machine::assemble(Instruction const& instr)
{
    using sixfive::AddressingMode;

    auto arg = instr;
    auto opcode = instr.opcode;

    auto const& instructions = sixfive::Machine<>::getInstructions();

    if (arg.mode == AddressingMode::ZP_REL) {
        auto bit = arg.val >> 24;
        arg.val &= 0xffffff;
        opcode = opcode + std::to_string(bit);
    }

    // Find a matching opcode
    auto it0 = std::find_if(instructions.begin(), instructions.end(),
                            [&](auto const& i) { return i.name == opcode; });

    if (it0 == instructions.end()) {
        return AsmResult::NoSuchOpcode;
    }

    auto modeMatches = [&](auto const& opcode, auto const& instruction) {
        if (instruction.mode == opcode.mode) {
            return true;
        }

        // Adressing mode did not match directly. See if we can
        // 'optimize it' depending on the instruction value

        static std::unordered_map<AddressingMode, AddressingMode> conv{
            {AddressingMode::INDZ, AddressingMode::IND},
            {AddressingMode::ZPX, AddressingMode::ABSX},
            {AddressingMode::ZPY, AddressingMode::ABSY},
            {AddressingMode::ZP, AddressingMode::ABS}};

        if (instruction.val >= 0 && instruction.val <= 0xff) {
            if (conv[opcode.mode] == instruction.mode) {
                return true;
            }
        }

        if (instruction.mode == AddressingMode::ABS &&
            opcode.mode == AddressingMode::REL) {
            return true;
        }
        return false;
    };

    // Find a matching addressing mode
    auto it1 = std::find_if(it0->opcodes.begin(), it0->opcodes.end(),
                            [&](auto const& o) { return modeMatches(o, arg); });

    if (it1 == it0->opcodes.end()) {
        return AsmResult::IllegalAdressingMode;
    }
    arg.mode = it1->mode;

    if (arg.mode == AddressingMode::REL) {
        arg.val = arg.val - currentSection->pc - 2;
    }

    if (arg.mode == AddressingMode::ZP_REL) {
        auto adr = arg.val & 0xffff;
        auto val = (arg.val >> 16) & 0xff;
        auto diff = adr - currentSection->pc - 2;
        arg.val = diff << 8 | val;
    }

    auto sz = opSize(arg.mode);

    auto v = arg.val & (sz == 2 ? 0xff : 0xffff);

    if (fp != nullptr) {
        if (inData) {
            fputs("\"\n", fp);
        }
        inData = false;
        fprintf(fp, "%04x : %s ", currentSection->pc, it0->name);
        if (arg.mode == sixfive::AddressingMode::REL) {
            v = (static_cast<int8_t>(v)) + 2 + currentSection->pc;
        }
        fprintf(fp, modeTemplate.at(arg.mode), v);
        fputs("\n", fp);
    }

    writeByte(it1->code);
    if (sz > 1) {
        writeByte(arg.val & 0xff);
    }
    if (sz > 2) {
        writeByte(arg.val >> 8);
    }

    if (arg.mode == AddressingMode::REL && (arg.val > 127 || arg.val < -128)) {
        return AsmResult::Truncated;
    }

    return AsmResult::Ok;
}

uint8_t Machine::readRam(uint16_t offset) const
{
    return machine->readMem(offset);
}
void Machine::writeRam(uint16_t offset, uint8_t val)
{
    machine->writeRam(offset, val);
}

void Machine::bankWriteFunction(uint16_t adr, uint8_t val, void* data)
{
    auto* thiz = static_cast<Machine*>(data);
    thiz->bank_write_functions[adr >> 8](adr, val);
}

uint8_t Machine::bankReadFunction(uint16_t adr, void* data)
{
    auto* thiz = static_cast<Machine*>(data);
    return thiz->bank_read_functions[adr >> 8](adr);
}

void Machine::setBankWrite(int bank, int len,
                           std::function<void(uint16_t, uint8_t)> const& fn)
{
    bank_write_functions[bank] = fn;
    machine->mapWriteCallback(bank, len, this, bankWriteFunction);
}

void Machine::setBankRead(int hi_adr, int len,
                          std::function<uint8_t(uint16_t)> const& fn)
{
    bank_read_functions[hi_adr] = fn;
    machine->mapReadCallback(hi_adr, len, this, bankReadFunction);
}

void Machine::setBankRead(int hi_adr, int len, int bank)
{
    Section const* bankSection = nullptr;
    int32_t adr = (bank << 16) | (hi_adr << 8);
    for (auto const& section : sections) {
        if (section.start == adr) {
            bankSection = &section;
            break;
        }
    }
    Check(bankSection != nullptr, "Could not map bank");
    machine->mapRom(hi_adr, bankSection->data.data(), len);
}

std::vector<uint8_t> Machine::getRam()
{
    std::vector<uint8_t> data(0x10000);
    machine->readRam(0, &data[0], data.size());
    return data;
}

Tuple6 Machine::getRegs() const
{
    return machine->regs();
}

void Machine::setRegs(Tuple6 const& regs)
{
    auto r = machine->regs();
    std::get<0>(r) = std::get<0>(regs);
    std::get<1>(r) = std::get<1>(regs);
    std::get<2>(r) = std::get<2>(regs);
    std::get<3>(r) = std::get<3>(regs);
    std::get<4>(r) = std::get<4>(regs);
    std::get<5>(r) = std::get<5>(regs);
}
