#pragma once

#include <cstddef>
#include <string>
#include <utility>

// Single-line editable text with a caret and selection. Replaces the previous
// "append on WM_CHAR, pop on backspace" model, which had no caret position at all.
class TextEditor
{
public:
    const std::wstring& text() const { return text_; }
    size_t caret() const { return caret_; }
    size_t anchor() const { return anchor_; }

    bool empty() const { return text_.empty(); }
    bool hasSelection() const { return caret_ != anchor_; }
    std::pair<size_t, size_t> selection() const;
    std::wstring selectedText() const;

    void setText(std::wstring value);
    void clear();

    bool insert(const std::wstring& value);
    bool insertChar(wchar_t ch);
    bool backspace(bool wholeWord);
    bool deleteForward(bool wholeWord);
    bool deleteSelection();

    void moveLeft(bool wholeWord, bool extend);
    void moveRight(bool wholeWord, bool extend);
    void moveHome(bool extend);
    void moveEnd(bool extend);
    void selectAll();

    // Caret placement coming back from a DirectWrite hit test.
    void placeCaret(size_t position, bool extend);
    void selectWordAt(size_t position);

private:
    void collapseTo(size_t position, bool extend);
    size_t previousWordBoundary(size_t from) const;
    size_t nextWordBoundary(size_t from) const;

    std::wstring text_;
    size_t caret_ = 0;
    size_t anchor_ = 0;
};
