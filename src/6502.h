#pragma once

#include <array>
#include <cstdint>

namespace sixfive {

// Addressing modes
enum class Mode : uint8_t
{
    NONE,
    ACC,
    IMM,
    REL,
    ZP,
    ZPX,
    ZPY,
    INDX,
    INDY,
    INDZ,
    IND,
    ABS,
    ABSX,
    ABSY,
    ZP_REL,
};

enum class Reg
{
    A,
    X,
    Y,
    SP,
    SR,
    PC
};


} // namespace sixfive
