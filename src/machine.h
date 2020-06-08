#pragma once

#include "defines.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

class machine_error : public std::exception
{
public:
    explicit machine_error(std::string m = "Target error") : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }

private:
    std::string msg;
};


namespace sixfive {
struct DefaultPolicy;
template <class POLICY>
struct Machine;
} // namespace sixfive

enum SectionFlags
{
    NoStorage = 1,
    WriteToDisk = 2,
    ReadOnly = 4
};

struct Section
{
    Section(std::string const& n, uint16_t s) : name(n), start(s), pc(s) {}
    std::string name;
    uint32_t start = 0;
    uint32_t pc = 0;
    uint32_t flags{};
    std::vector<uint8_t> data;
};

enum class OutFmt { Prg, Raw };

class Machine
{
public:
    struct State
    {
        uint32_t pc;
        size_t offset;
    };

    Machine();
    ~Machine();

    void clear();

    uint32_t writeByte(uint8_t b);
    int assemble(Instruction const& instr);
    Section& addSection(std::string const& name, uint32_t start);
    void setSection(std::string const& name);
    Section const& getSection(std::string const& name) const;
    Section& getCurrentSection();
    std::deque<Section> const& getSections() const { return sections; }
    State saveState() const;
    void restoreState(State const& s);
    uint32_t getPC() const;
    void write(std::string const& name, OutFmt fmt);
    void setOutput(FILE* f);

    uint8_t readRam(uint16_t offset) const;

    uint32_t run(uint16_t pc);
    std::vector<uint8_t> getRam();
    std::tuple<unsigned, unsigned, unsigned, unsigned, unsigned, unsigned>
    getRegs() const;

private:
    std::unique_ptr<sixfive::Machine<sixfive::DefaultPolicy>> machine;
    std::deque<Section> sections;
    Section* currentSection = nullptr;
    FILE* fp = stdout;
};
