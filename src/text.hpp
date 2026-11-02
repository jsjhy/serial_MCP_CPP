// Incremental terminal decoding: split UTF-8, CSI SGR foreground colors, OSC filtering and CR/LF merging.
// Device cursor movement/clearing is not emulated. Display replaces partial UTF-16 lines; logs consume deltas.
#pragma once
#include "common.hpp"

namespace sm
{
enum class Direction
{
    Rx,
    Tx,
    Status
};

struct Record
{
    Direction dir;
    std::string data;
    std::string time;
};

struct Piece
{
    std::wstring text;
    COLORREF color;
};

struct Patch
{
    long erase = 0;
    std::vector<Piece> pieces;
};

class TextParser
{
    int escape_ = 0;
    std::string sequence_;
    std::string utf_;
    bool cr_ = false;
    COLORREF fg_;
    COLORREF default_;
    void sgr();

public:
    using Emit = std::function<void(const std::wstring&, COLORREF)>;

    explicit TextParser(COLORREF fg)
        : fg_(fg),
          default_(fg)
    {
    }

    void feed(const std::string& bytes, const Emit& emit);
    void finish(const Emit& emit);
};

class Display
{
    Palette palette_;
    TextParser parser_;
    std::vector<Piece> line_;
    long previous_ = 0;
    long body_ = 0;
    std::string time_;
    void add(const std::wstring& text, COLORREF color);

public:
    explicit Display(Palette p)
        : palette_(p),
          parser_(p.fg)
    {
    }

    Patch feed(const Record& record, bool hex);

    void reset()
    {
        line_.clear();
        previous_ = body_ = 0;
        time_.clear();
        parser_ = TextParser(palette_.fg);
    }
};
} // namespace sm
