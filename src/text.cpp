// Text processing does not touch windows or files. All indices and patch lengths use Windows UTF-16 units.
#include "text.hpp"

namespace sm
{
void TextParser::sgr()
{
    std::vector<int> p;
    std::istringstream in(sequence_);
    std::string s;
    while (std::getline(in, s, ';'))
    {
        try
        {
            p.push_back(s.empty() ? 0 : std::stoi(s));
        }
        catch (...)
        {
            p.push_back(-1);
        }
    }
    if (p.empty())
    {
        p.push_back(0);
    }
    for (size_t i = 0; i < p.size(); ++i)
    {
        int c = p[i];
        if (c == 0 || c == 39)
        {
            fg_ = default_;
        }
        else if (c >= 30 && c <= 37)
        {
            fg_ = Palette::ansi(c - 30);
        }
        else if (c >= 90 && c <= 97)
        {
            fg_ = Palette::ansi(c - 90 + 8);
        }
        else if (c == 38 && i + 2 < p.size() && p[i + 1] == 5)
        {
            fg_ = Palette::ansi(std::clamp(p[i + 2], 0, 255));
            i += 2;
        }
        else if (c == 38 && i + 4 < p.size() && p[i + 1] == 2)
        {
            fg_ =
                RGB(std::clamp(p[i + 2], 0, 255),
                    std::clamp(p[i + 3], 0, 255),
                    std::clamp(p[i + 4], 0, 255));
            i += 4;
        }
    }
}

void TextParser::finish(const Emit& emit)
{
    if (!utf_.empty())
    {
        for (unsigned char b : utf_)
        {
            emit(widen("\\x" + hex_text(std::string(1, char(b)))), fg_);
        }
        utf_.clear();
    }
    escape_ = 0;
    sequence_.clear();
}

void TextParser::feed(const std::string& bytes, const Emit& emit)
{
    for (unsigned char b : bytes)
    {
        if (escape_)
        {
            if (sequence_.size() > 4096)
            {
                escape_ = 0;
                sequence_.clear();
            }
            else if (escape_ == 1)
            {
                if (b == '[')
                {
                    escape_ = 2;
                    sequence_.clear();
                }
                else if (b == ']')
                {
                    escape_ = 3;
                    sequence_.clear();
                }
                else
                {
                    escape_ = 0;
                }
                continue;
            }
            else if (escape_ == 2)
            {
                if (b >= 0x40 && b <= 0x7e)
                {
                    if (b == 'm')
                    {
                        sgr();
                    }
                    escape_ = 0;
                    sequence_.clear();
                }
                else
                {
                    sequence_ += char(b);
                }
                continue;
            }
            else
            {
                sequence_ += char(b);
                if (b == 7 || (escape_ == 4 && b == '\\'))
                {
                    escape_ = 0;
                    sequence_.clear();
                }
                else
                {
                    escape_ = b == 27 ? 4 : 3;
                }
                continue;
            }
        }
        if (b == 27)
        {
            finish(emit);
            escape_ = 1;
            continue;
        }
        if (b < 128)
        {
            if (!utf_.empty())
            {
                finish(emit);
            }
            if (b == '\r')
            {
                emit(L"\r", fg_);
                cr_ = true;
                continue;
            }
            if (b == '\n')
            {
                if (!cr_)
                {
                    emit(L"\r", fg_);
                }
                cr_ = false;
                continue;
            }
            cr_ = false;
            if (b == '\t' || b == '\b' || (b >= 32 && b < 127))
            {
                emit(std::wstring(1, wchar_t(b)), fg_);
            }
            else if (b != 7)
            {
                emit(widen("\\x" + hex_text(std::string(1, char(b)))), fg_);
            }
            continue;
        }
        cr_ = false;
        utf_ += char(b);
        unsigned char first = utf_[0];
        size_t need = first >= 0xf0 && first <= 0xf4   ? 4
                      : first >= 0xe0 && first <= 0xef ? 3
                      : first >= 0xc2 && first <= 0xdf ? 2
                                                       : 1;
        if (utf_.size() >= need)
        {
            try
            {
                emit(widen(utf_), fg_);
                utf_.clear();
            }
            catch (...)
            {
                finish(emit);
            }
        }
    }
}

void Display::add(const std::wstring& text, COLORREF color)
{
    if (!line_.empty() && line_.back().color == color)
    {
        line_.back().text += text;
    }
    else
    {
        line_.push_back({text, color});
    }
}

Patch Display::feed(const Record& rec, bool hex)
{
    Patch out;
    out.erase = previous_;
    auto prefix = [&]
    {
        if (line_.empty())
        {
            if (time_.empty())
            {
                time_ = rec.time;
            }
            add(widen("[" + time_ + "] "), palette_.dim);
        }
    };
    auto finish_line = [&]
    {
        prefix();
        for (auto& p : line_)
        {
            out.pieces.push_back(std::move(p));
        }
        out.pieces.push_back({L"\r", palette_.fg});
        line_.clear();
        time_.clear();
        body_ = 0;
    };
    if (rec.dir == Direction::Rx && !hex)
    {
        parser_.feed(
            rec.data,
            [&](const std::wstring& s, COLORREF color)
            {
                if (s == L"\r")
                {
                    finish_line();
                    return;
                }
                if (s == L"\b")
                {
                    if (body_ > 0 && !line_.empty())
                    {
                        auto& t = line_.back().text;
                        size_t n = 1;
                        if (!t.empty() && t.back() >= 0xdc00 && t.back() <= 0xdfff)
                        {
                            n = 2;
                        }
                        t.resize(t.size() - std::min(n, t.size()));
                        body_ -= long(n);
                        if (t.empty())
                        {
                            line_.pop_back();
                        }
                    }
                    return;
                }
                prefix();
                if (body_ > 16384)
                {
                    finish_line();
                }
                prefix();
                if (s == L"\t")
                {
                    std::wstring spaces(8 - (body_ % 8), L' ');
                    body_ += long(spaces.size());
                    add(spaces, color);
                }
                else
                {
                    body_ += long(s.size());
                    add(s, color);
                }
            }
        );
    }
    else
    {
        if (!line_.empty())
        {
            finish_line();
        }
        parser_ = TextParser(palette_.fg);
        prefix();
        if (rec.dir == Direction::Tx)
        {
            add(L"TX: ", palette_.fg);
        }
        else if (rec.dir == Direction::Status)
        {
            add(L"STATUS: ", palette_.fg);
        }
        if (hex)
        {
            add(widen(hex_text(rec.data)), palette_.fg);
        }
        else
        {
            TextParser p(palette_.fg);
            p.feed(
                rec.data,
                [&](auto& s, auto c)
                {
                    if (s != L"\r" && s != L"\b")
                    {
                        add(s, c);
                    }
                }
            );
            p.finish(
                [&](auto& s, auto c)
                {
                    add(s, c);
                }
            );
        }
        finish_line();
    }
    previous_ = 0;
    for (auto& p : line_)
    {
        out.pieces.push_back(p);
        previous_ += long(p.text.size());
    }
    return out;
}
} // namespace sm
