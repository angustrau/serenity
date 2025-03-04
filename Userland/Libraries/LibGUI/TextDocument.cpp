/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibCore/Timer.h>
#include <LibGUI/TextDocument.h>
#include <LibRegex/Regex.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Segmentation.h>

namespace GUI {

NonnullRefPtr<TextDocument> TextDocument::create(Client* client)
{
    return adopt_ref(*new TextDocument(client));
}

TextDocument::TextDocument(Client* client)
{
    if (client)
        m_clients.set(client);
    append_line(make<TextDocumentLine>(*this));
    set_unmodified();

    m_undo_stack.on_state_change = [this] {
        if (m_client_notifications_enabled) {
            for (auto* client : m_clients)
                client->document_did_update_undo_stack();
        }
    };
}

bool TextDocument::set_text(StringView text, AllowCallback allow_callback)
{
    m_client_notifications_enabled = false;
    m_undo_stack.clear();
    m_spans.clear();
    m_folding_regions.clear();
    remove_all_lines();

    ArmedScopeGuard clear_text_guard([this]() {
        set_text({});
    });

    size_t start_of_current_line = 0;

    auto add_line = [&](size_t current_position) -> bool {
        size_t line_length = current_position - start_of_current_line;
        auto line = make<TextDocumentLine>(*this);

        bool success = true;
        if (line_length)
            success = line->set_text(*this, text.substring_view(start_of_current_line, current_position - start_of_current_line));

        if (!success)
            return false;

        append_line(move(line));
        start_of_current_line = current_position + 1;

        return true;
    };

    size_t i = 0;
    for (i = 0; i < text.length(); ++i) {
        if (text[i] != '\n')
            continue;

        auto success = add_line(i);
        if (!success)
            return false;
    }

    auto success = add_line(i);
    if (!success)
        return false;

    // Don't show the file's trailing newline as an actual new line.
    if (line_count() > 1 && line(line_count() - 1).is_empty())
        (void)m_lines.take_last();

    m_client_notifications_enabled = true;

    for (auto* client : m_clients)
        client->document_did_set_text(allow_callback);

    clear_text_guard.disarm();

    // FIXME: Should the modified state be cleared on some of the earlier returns as well?
    set_unmodified();
    return true;
}

size_t TextDocumentLine::first_non_whitespace_column() const
{
    for (size_t i = 0; i < length(); ++i) {
        auto code_point = code_points()[i];
        if (!is_ascii_space(code_point))
            return i;
    }
    return length();
}

Optional<size_t> TextDocumentLine::last_non_whitespace_column() const
{
    for (ssize_t i = length() - 1; i >= 0; --i) {
        auto code_point = code_points()[i];
        if (!is_ascii_space(code_point))
            return i;
    }
    return {};
}

bool TextDocumentLine::ends_in_whitespace() const
{
    if (!length())
        return false;
    return is_ascii_space(code_points()[length() - 1]);
}

bool TextDocumentLine::can_select() const
{
    if (is_empty())
        return false;
    for (size_t i = 0; i < length(); ++i) {
        auto code_point = code_points()[i];
        if (code_point != '\n' && code_point != '\r' && code_point != '\f' && code_point != '\v')
            return true;
    }
    return false;
}

size_t TextDocumentLine::leading_spaces() const
{
    size_t count = 0;
    for (; count < m_text.size(); ++count) {
        if (m_text[count] != ' ') {
            break;
        }
    }
    return count;
}

DeprecatedString TextDocumentLine::to_utf8() const
{
    StringBuilder builder;
    builder.append(view());
    return builder.to_deprecated_string();
}

TextDocumentLine::TextDocumentLine(TextDocument& document)
{
    clear(document);
}

TextDocumentLine::TextDocumentLine(TextDocument& document, StringView text)
{
    set_text(document, text);
}

void TextDocumentLine::clear(TextDocument& document)
{
    m_text.clear();
    document.update_views({});
}

void TextDocumentLine::set_text(TextDocument& document, Vector<u32> const text)
{
    m_text = move(text);
    document.update_views({});
}

bool TextDocumentLine::set_text(TextDocument& document, StringView text)
{
    if (text.is_empty()) {
        clear(document);
        return true;
    }
    m_text.clear();
    Utf8View utf8_view(text);
    if (!utf8_view.validate()) {
        return false;
    }
    for (auto code_point : utf8_view)
        m_text.append(code_point);
    document.update_views({});
    return true;
}

void TextDocumentLine::append(TextDocument& document, u32 const* code_points, size_t length)
{
    if (length == 0)
        return;
    m_text.append(code_points, length);
    document.update_views({});
}

void TextDocumentLine::append(TextDocument& document, u32 code_point)
{
    insert(document, length(), code_point);
}

void TextDocumentLine::prepend(TextDocument& document, u32 code_point)
{
    insert(document, 0, code_point);
}

void TextDocumentLine::insert(TextDocument& document, size_t index, u32 code_point)
{
    if (index == length()) {
        m_text.append(code_point);
    } else {
        m_text.insert(index, code_point);
    }
    document.update_views({});
}

void TextDocumentLine::remove(TextDocument& document, size_t index)
{
    if (index == length()) {
        m_text.take_last();
    } else {
        m_text.remove(index);
    }
    document.update_views({});
}

void TextDocumentLine::remove_range(TextDocument& document, size_t start, size_t length)
{
    VERIFY(length <= m_text.size());

    Vector<u32> new_data;
    new_data.ensure_capacity(m_text.size() - length);
    for (size_t i = 0; i < start; ++i)
        new_data.append(m_text[i]);
    for (size_t i = (start + length); i < m_text.size(); ++i)
        new_data.append(m_text[i]);
    m_text = move(new_data);
    document.update_views({});
}

void TextDocumentLine::keep_range(TextDocument& document, size_t start_index, size_t length)
{
    VERIFY(start_index + length < m_text.size());

    Vector<u32> new_data;
    new_data.ensure_capacity(m_text.size());
    for (size_t i = start_index; i <= (start_index + length); i++)
        new_data.append(m_text[i]);

    m_text = move(new_data);
    document.update_views({});
}

void TextDocumentLine::truncate(TextDocument& document, size_t length)
{
    m_text.resize(length);
    document.update_views({});
}

void TextDocument::append_line(NonnullOwnPtr<TextDocumentLine> line)
{
    lines().append(move(line));
    if (m_client_notifications_enabled) {
        for (auto* client : m_clients)
            client->document_did_append_line();
    }
}

void TextDocument::insert_line(size_t line_index, NonnullOwnPtr<TextDocumentLine> line)
{
    lines().insert(line_index, move(line));
    if (m_client_notifications_enabled) {
        for (auto* client : m_clients)
            client->document_did_insert_line(line_index);
    }
}

NonnullOwnPtr<TextDocumentLine> TextDocument::take_line(size_t line_index)
{
    auto line = lines().take(line_index);
    if (m_client_notifications_enabled) {
        for (auto* client : m_clients)
            client->document_did_remove_line(line_index);
    }
    return line;
}

void TextDocument::remove_line(size_t line_index)
{
    lines().remove(line_index);
    if (m_client_notifications_enabled) {
        for (auto* client : m_clients)
            client->document_did_remove_line(line_index);
    }
}

void TextDocument::remove_all_lines()
{
    lines().clear();
    if (m_client_notifications_enabled) {
        for (auto* client : m_clients)
            client->document_did_remove_all_lines();
    }
}

void TextDocument::register_client(Client& client)
{
    m_clients.set(&client);
}

void TextDocument::unregister_client(Client& client)
{
    m_clients.remove(&client);
}

void TextDocument::update_views(Badge<TextDocumentLine>)
{
    notify_did_change();
}

void TextDocument::notify_did_change()
{
    if (m_client_notifications_enabled) {
        for (auto* client : m_clients)
            client->document_did_change();
    }

    m_regex_needs_update = true;
}

void TextDocument::set_all_cursors(TextPosition const& position)
{
    if (m_client_notifications_enabled) {
        for (auto* client : m_clients)
            client->document_did_set_cursor(position);
    }
}

DeprecatedString TextDocument::text() const
{
    StringBuilder builder;
    for (size_t i = 0; i < line_count(); ++i) {
        auto& line = this->line(i);
        builder.append(line.view());
        if (i != line_count() - 1)
            builder.append('\n');
    }
    return builder.to_deprecated_string();
}

DeprecatedString TextDocument::text_in_range(TextRange const& a_range) const
{
    auto range = a_range.normalized();
    if (is_empty() || line_count() < range.end().line() - range.start().line())
        return DeprecatedString("");

    StringBuilder builder;
    for (size_t i = range.start().line(); i <= range.end().line(); ++i) {
        auto& line = this->line(i);
        size_t selection_start_column_on_line = range.start().line() == i ? range.start().column() : 0;
        size_t selection_end_column_on_line = range.end().line() == i ? range.end().column() : line.length();

        if (!line.is_empty()) {
            builder.append(
                Utf32View(
                    line.code_points() + selection_start_column_on_line,
                    selection_end_column_on_line - selection_start_column_on_line));
        }

        if (i != range.end().line())
            builder.append('\n');
    }

    return builder.to_deprecated_string();
}

// This function will return the position of the previous grapheme cluster
// break, relative to the cursor, for "correct looking" parsing of unicode based
// on grapheme cluster boundary algorithm.
size_t TextDocument::get_previous_grapheme_cluster_boundary(TextPosition const& cursor) const
{
    if (!cursor.is_valid())
        return 0;

    auto const& line = this->line(cursor.line());

    auto index = Unicode::previous_grapheme_segmentation_boundary(line.view(), cursor.column());
    return index.value_or(cursor.column() - 1);
}

// This function will return the position of the next grapheme cluster break,
// relative to the cursor, for "correct looking" parsing of unicode based on
// grapheme cluster boundary algorithm.
size_t TextDocument::get_next_grapheme_cluster_boundary(TextPosition const& cursor) const
{
    if (!cursor.is_valid())
        return 0;

    auto const& line = this->line(cursor.line());

    auto index = Unicode::next_grapheme_segmentation_boundary(line.view(), cursor.column());
    return index.value_or(cursor.column() + 1);
}

u32 TextDocument::code_point_at(TextPosition const& position) const
{
    VERIFY(position.line() < line_count());
    auto& line = this->line(position.line());
    if (position.column() == line.length())
        return '\n';
    return line.code_points()[position.column()];
}

TextPosition TextDocument::next_position_after(TextPosition const& position, SearchShouldWrap should_wrap) const
{
    auto& line = this->line(position.line());
    if (position.column() == line.length()) {
        if (position.line() == line_count() - 1) {
            if (should_wrap == SearchShouldWrap::Yes)
                return { 0, 0 };
            return {};
        }
        return { position.line() + 1, 0 };
    }
    return { position.line(), position.column() + 1 };
}

TextPosition TextDocument::previous_position_before(TextPosition const& position, SearchShouldWrap should_wrap) const
{
    if (position.column() == 0) {
        if (position.line() == 0) {
            if (should_wrap == SearchShouldWrap::Yes) {
                auto& last_line = this->line(line_count() - 1);
                return { line_count() - 1, last_line.length() };
            }
            return {};
        }
        auto& prev_line = this->line(position.line() - 1);
        return { position.line() - 1, prev_line.length() };
    }
    return { position.line(), position.column() - 1 };
}

void TextDocument::update_regex_matches(StringView needle)
{
    if (m_regex_needs_update || needle != m_regex_needle) {
        Regex<PosixExtended> re(needle);

        Vector<RegexStringView> views;

        for (size_t line = 0; line < m_lines.size(); ++line) {
            views.append(m_lines[line]->view());
        }
        re.search(views, m_regex_result);
        m_regex_needs_update = false;
        m_regex_needle = DeprecatedString { needle };
        m_regex_result_match_index = -1;
        m_regex_result_match_capture_group_index = -1;
    }
}

TextRange TextDocument::find_next(StringView needle, TextPosition const& start, SearchShouldWrap should_wrap, bool regmatch, bool match_case)
{
    if (needle.is_empty())
        return {};

    if (regmatch) {
        if (!m_regex_result.matches.size())
            return {};

        regex::Match match;
        bool use_whole_match { false };

        auto next_match = [&] {
            m_regex_result_match_capture_group_index = 0;
            if (m_regex_result_match_index == m_regex_result.matches.size() - 1) {
                if (should_wrap == SearchShouldWrap::Yes)
                    m_regex_result_match_index = 0;
                else
                    ++m_regex_result_match_index;
            } else
                ++m_regex_result_match_index;
        };

        if (m_regex_result.n_capture_groups) {
            if (m_regex_result_match_index >= m_regex_result.capture_group_matches.size())
                next_match();
            else {
                // check if last capture group has been reached
                if (m_regex_result_match_capture_group_index >= m_regex_result.capture_group_matches.at(m_regex_result_match_index).size()) {
                    next_match();
                } else {
                    // get to the next capture group item
                    ++m_regex_result_match_capture_group_index;
                }
            }

            // use whole match, if there is no capture group for current index
            if (m_regex_result_match_index >= m_regex_result.capture_group_matches.size())
                use_whole_match = true;
            else if (m_regex_result_match_capture_group_index >= m_regex_result.capture_group_matches.at(m_regex_result_match_index).size())
                next_match();

        } else {
            next_match();
        }

        if (use_whole_match || !m_regex_result.capture_group_matches.at(m_regex_result_match_index).size())
            match = m_regex_result.matches.at(m_regex_result_match_index);
        else
            match = m_regex_result.capture_group_matches.at(m_regex_result_match_index).at(m_regex_result_match_capture_group_index);

        return TextRange { { match.line, match.column }, { match.line, match.column + match.view.length() } };
    }

    TextPosition position = start.is_valid() ? start : TextPosition(0, 0);
    TextPosition original_position = position;

    TextPosition start_of_potential_match;
    size_t needle_index = 0;

    Utf8View unicode_needle(needle);
    Vector<u32> needle_code_points;
    for (u32 code_point : unicode_needle)
        needle_code_points.append(code_point);

    do {
        auto ch = code_point_at(position);

        bool code_point_matches = false;
        if (needle_index >= needle_code_points.size())
            code_point_matches = false;
        else if (match_case)
            code_point_matches = ch == needle_code_points[needle_index];
        else
            code_point_matches = Unicode::to_unicode_lowercase(ch) == Unicode::to_unicode_lowercase(needle_code_points[needle_index]);

        if (code_point_matches) {
            if (needle_index == 0)
                start_of_potential_match = position;
            ++needle_index;
            if (needle_index >= needle_code_points.size())
                return { start_of_potential_match, next_position_after(position, should_wrap) };
        } else {
            if (needle_index > 0)
                position = start_of_potential_match;
            needle_index = 0;
        }
        position = next_position_after(position, should_wrap);
    } while (position.is_valid() && position != original_position);

    return {};
}

TextRange TextDocument::find_previous(StringView needle, TextPosition const& start, SearchShouldWrap should_wrap, bool regmatch, bool match_case)
{
    if (needle.is_empty())
        return {};

    if (regmatch) {
        if (!m_regex_result.matches.size())
            return {};

        regex::Match match;
        bool use_whole_match { false };

        auto next_match = [&] {
            if (m_regex_result_match_index == 0) {
                if (should_wrap == SearchShouldWrap::Yes)
                    m_regex_result_match_index = m_regex_result.matches.size() - 1;
                else
                    --m_regex_result_match_index;
            } else
                --m_regex_result_match_index;

            m_regex_result_match_capture_group_index = m_regex_result.capture_group_matches.at(m_regex_result_match_index).size() - 1;
        };

        if (m_regex_result.n_capture_groups) {
            if (m_regex_result_match_index >= m_regex_result.capture_group_matches.size())
                next_match();
            else {
                // check if last capture group has been reached
                if (m_regex_result_match_capture_group_index >= m_regex_result.capture_group_matches.at(m_regex_result_match_index).size()) {
                    next_match();
                } else {
                    // get to the next capture group item
                    --m_regex_result_match_capture_group_index;
                }
            }

            // use whole match, if there is no capture group for current index
            if (m_regex_result_match_index >= m_regex_result.capture_group_matches.size())
                use_whole_match = true;
            else if (m_regex_result_match_capture_group_index >= m_regex_result.capture_group_matches.at(m_regex_result_match_index).size())
                next_match();

        } else {
            next_match();
        }

        if (use_whole_match || !m_regex_result.capture_group_matches.at(m_regex_result_match_index).size())
            match = m_regex_result.matches.at(m_regex_result_match_index);
        else
            match = m_regex_result.capture_group_matches.at(m_regex_result_match_index).at(m_regex_result_match_capture_group_index);

        return TextRange { { match.line, match.column }, { match.line, match.column + match.view.length() } };
    }

    TextPosition position = start.is_valid() ? start : TextPosition(0, 0);
    position = previous_position_before(position, should_wrap);
    if (position.line() >= line_count())
        return {};
    TextPosition original_position = position;

    Utf8View unicode_needle(needle);
    Vector<u32> needle_code_points;
    for (u32 code_point : unicode_needle)
        needle_code_points.append(code_point);

    TextPosition end_of_potential_match;
    size_t needle_index = needle_code_points.size() - 1;

    do {
        auto ch = code_point_at(position);

        bool code_point_matches = false;
        if (needle_index >= needle_code_points.size())
            code_point_matches = false;
        else if (match_case)
            code_point_matches = ch == needle_code_points[needle_index];
        else
            code_point_matches = Unicode::to_unicode_lowercase(ch) == Unicode::to_unicode_lowercase(needle_code_points[needle_index]);

        if (code_point_matches) {
            if (needle_index == needle_code_points.size() - 1)
                end_of_potential_match = position;
            if (needle_index == 0)
                return { position, next_position_after(end_of_potential_match, should_wrap) };
            --needle_index;
        } else {
            if (needle_index < needle_code_points.size() - 1)
                position = end_of_potential_match;
            needle_index = needle_code_points.size() - 1;
        }
        position = previous_position_before(position, should_wrap);
    } while (position.is_valid() && position != original_position);

    return {};
}

Vector<TextRange> TextDocument::find_all(StringView needle, bool regmatch, bool match_case)
{
    Vector<TextRange> ranges;

    TextPosition position;
    for (;;) {
        auto range = find_next(needle, position, SearchShouldWrap::No, regmatch, match_case);
        if (!range.is_valid())
            break;
        ranges.append(range);
        position = range.end();
    }
    return ranges;
}

Optional<TextDocumentSpan> TextDocument::first_non_skippable_span_before(TextPosition const& position) const
{
    for (int i = m_spans.size() - 1; i >= 0; --i) {
        if (!m_spans[i].range.contains(position))
            continue;
        while ((i - 1) >= 0 && m_spans[i - 1].is_skippable)
            --i;
        if (i <= 0)
            return {};
        return m_spans[i - 1];
    }
    return {};
}

Optional<TextDocumentSpan> TextDocument::first_non_skippable_span_after(TextPosition const& position) const
{
    size_t i = 0;
    // Find the first span containing the cursor
    for (; i < m_spans.size(); ++i) {
        if (m_spans[i].range.contains(position))
            break;
    }
    // Find the first span *after* the cursor
    // TODO: For a large number of spans, binary search would be faster.
    for (; i < m_spans.size(); ++i) {
        if (!m_spans[i].range.contains(position))
            break;
    }
    // Skip skippable spans
    for (; i < m_spans.size(); ++i) {
        if (!m_spans[i].is_skippable)
            break;
    }
    if (i < m_spans.size())
        return m_spans[i];
    return {};
}

static bool should_continue_beyond_word(Utf32View const& view)
{
    static auto punctuation = Unicode::general_category_from_string("Punctuation"sv);
    static auto separator = Unicode::general_category_from_string("Separator"sv);

    if (!punctuation.has_value() || !separator.has_value())
        return false;

    auto has_any_gc = [&](auto code_point, auto&&... categories) {
        return (Unicode::code_point_has_general_category(code_point, *categories) || ...);
    };

    for (auto code_point : view) {
        if (!has_any_gc(code_point, punctuation, separator))
            return false;
    }

    return true;
}

TextPosition TextDocument::first_word_break_before(TextPosition const& position, bool start_at_column_before) const
{
    if (position.column() == 0) {
        if (position.line() == 0) {
            return TextPosition(0, 0);
        }
        auto previous_line = this->line(position.line() - 1);
        return TextPosition(position.line() - 1, previous_line.length());
    }

    auto target = position;
    auto const& line = this->line(target.line());

    auto modifier = start_at_column_before ? 1 : 0;
    if (target.column() == line.length())
        modifier = 1;

    target.set_column(target.column() - modifier);

    while (target.column() > 0) {
        if (auto index = Unicode::previous_word_segmentation_boundary(line.view(), target.column()); index.has_value()) {
            auto view_between_target_and_index = line.view().substring_view(*index, target.column() - *index);

            if (should_continue_beyond_word(view_between_target_and_index)) {
                target.set_column(*index == 0 ? 0 : *index - 1);
                continue;
            }

            target.set_column(*index);
            break;
        }
    }

    return target;
}

TextPosition TextDocument::first_word_break_after(TextPosition const& position) const
{
    auto target = position;
    auto const& line = this->line(target.line());

    if (position.column() >= line.length()) {
        if (position.line() >= this->line_count() - 1) {
            return position;
        }
        return TextPosition(position.line() + 1, 0);
    }

    while (target.column() < line.length()) {
        if (auto index = Unicode::next_word_segmentation_boundary(line.view(), target.column()); index.has_value()) {
            auto view_between_target_and_index = line.view().substring_view(target.column(), *index - target.column());

            if (should_continue_beyond_word(view_between_target_and_index)) {
                target.set_column(min(*index + 1, line.length()));
                continue;
            }

            target.set_column(*index);
            break;
        }
    }

    return target;
}

TextPosition TextDocument::first_word_before(TextPosition const& position, bool start_at_column_before) const
{
    if (position.column() == 0) {
        if (position.line() == 0) {
            return TextPosition(0, 0);
        }
        auto previous_line = this->line(position.line() - 1);
        return TextPosition(position.line() - 1, previous_line.length());
    }

    auto target = position;
    auto line = this->line(target.line());
    if (target.column() == line.length())
        start_at_column_before = 1;

    auto nonblank_passed = !is_ascii_blank(line.code_points()[target.column() - start_at_column_before]);
    while (target.column() > 0) {
        auto prev_code_point = line.code_points()[target.column() - 1];
        nonblank_passed |= !is_ascii_blank(prev_code_point);

        if (nonblank_passed && is_ascii_blank(prev_code_point)) {
            break;
        } else if (is_ascii_punctuation(prev_code_point)) {
            target.set_column(target.column() - 1);
            break;
        }

        target.set_column(target.column() - 1);
    }

    return target;
}

void TextDocument::undo()
{
    if (!can_undo())
        return;
    m_undo_stack.undo();
    notify_did_change();
}

void TextDocument::redo()
{
    if (!can_redo())
        return;
    m_undo_stack.redo();
    notify_did_change();
}

void TextDocument::add_to_undo_stack(NonnullOwnPtr<TextDocumentUndoCommand> undo_command)
{
    m_undo_stack.push(move(undo_command));
}

TextDocumentUndoCommand::TextDocumentUndoCommand(TextDocument& document)
    : m_document(document)
{
}

InsertTextCommand::InsertTextCommand(TextDocument& document, DeprecatedString const& text, TextPosition const& position)
    : TextDocumentUndoCommand(document)
    , m_text(text)
    , m_range({ position, position })
{
}

DeprecatedString InsertTextCommand::action_text() const
{
    return "Insert Text";
}

bool InsertTextCommand::merge_with(GUI::Command const& other)
{
    if (!is<InsertTextCommand>(other) || commit_time_expired())
        return false;

    auto const& typed_other = static_cast<InsertTextCommand const&>(other);
    if (typed_other.m_text.is_whitespace() && !m_text.is_whitespace())
        return false; // Skip if other is whitespace while this is not

    if (m_range.end() != typed_other.m_range.start())
        return false;
    if (m_range.start().line() != m_range.end().line())
        return false;

    StringBuilder builder(m_text.length() + typed_other.m_text.length());
    builder.append(m_text);
    builder.append(typed_other.m_text);
    m_text = builder.to_deprecated_string();
    m_range.set_end(typed_other.m_range.end());

    m_timestamp = MonotonicTime::now();
    return true;
}

void InsertTextCommand::perform_formatting(TextDocument::Client const& client)
{
    const size_t tab_width = client.soft_tab_width();
    auto const& dest_line = m_document.line(m_range.start().line());
    bool const should_auto_indent = client.is_automatic_indentation_enabled();

    StringBuilder builder;
    size_t column = m_range.start().column();
    size_t line_indentation = dest_line.leading_spaces();
    bool at_start_of_line = line_indentation == column;

    for (auto input_char : m_text) {
        if (input_char == '\n') {
            size_t spaces_at_end = 0;
            if (column < line_indentation)
                spaces_at_end = line_indentation - column;
            line_indentation -= spaces_at_end;
            builder.append('\n');
            column = 0;
            if (should_auto_indent) {
                for (; column < line_indentation; ++column) {
                    builder.append(' ');
                }
            }
            at_start_of_line = true;
        } else if (input_char == '\t') {
            size_t next_soft_tab_stop = ((column + tab_width) / tab_width) * tab_width;
            size_t spaces_to_insert = next_soft_tab_stop - column;
            for (size_t i = 0; i < spaces_to_insert; ++i) {
                builder.append(' ');
            }
            column = next_soft_tab_stop;
            if (at_start_of_line) {
                line_indentation = column;
            }
        } else {
            if (input_char == ' ') {
                if (at_start_of_line) {
                    ++line_indentation;
                }
            } else {
                at_start_of_line = false;
            }
            builder.append(input_char);
            ++column;
        }
    }
    m_text = builder.to_deprecated_string();
}

void InsertTextCommand::redo()
{
    auto new_cursor = m_document.insert_at(m_range.start(), m_text, m_client);
    // NOTE: We don't know where the range ends until after doing redo().
    //       This is okay since we always do redo() after adding this to the undo stack.
    m_range.set_end(new_cursor);
    m_document.set_all_cursors(new_cursor);
}

void InsertTextCommand::undo()
{
    m_document.remove(m_range);
    m_document.set_all_cursors(m_range.start());
}

RemoveTextCommand::RemoveTextCommand(TextDocument& document, DeprecatedString const& text, TextRange const& range, TextPosition const& original_cursor_position)
    : TextDocumentUndoCommand(document)
    , m_text(text)
    , m_range(range)
    , m_original_cursor_position(original_cursor_position)
{
}

DeprecatedString RemoveTextCommand::action_text() const
{
    return "Remove Text";
}

bool RemoveTextCommand::merge_with(GUI::Command const& other)
{
    if (!is<RemoveTextCommand>(other) || commit_time_expired())
        return false;

    auto const& typed_other = static_cast<RemoveTextCommand const&>(other);

    if (m_range.start() != typed_other.m_range.end())
        return false;
    if (m_range.start().line() != m_range.end().line())
        return false;

    // Merge backspaces
    StringBuilder builder(m_text.length() + typed_other.m_text.length());
    builder.append(typed_other.m_text);
    builder.append(m_text);
    m_text = builder.to_deprecated_string();
    m_range.set_start(typed_other.m_range.start());

    m_timestamp = MonotonicTime::now();
    return true;
}

void RemoveTextCommand::redo()
{
    m_document.remove(m_range);
    m_document.set_all_cursors(m_range.start());
}

void RemoveTextCommand::undo()
{
    m_document.insert_at(m_range.start(), m_text);
    m_document.set_all_cursors(m_original_cursor_position);
}

InsertLineCommand::InsertLineCommand(TextDocument& document, TextPosition cursor, DeprecatedString&& text, InsertPosition pos)
    : TextDocumentUndoCommand(document)
    , m_cursor(cursor)
    , m_text(move(text))
    , m_pos(pos)
{
}

void InsertLineCommand::redo()
{
    size_t line_number = compute_line_number();
    m_document.insert_line(line_number, make<TextDocumentLine>(m_document, m_text));
    m_document.set_all_cursors(TextPosition { line_number, m_document.line(line_number).length() });
}

void InsertLineCommand::undo()
{
    size_t line_number = compute_line_number();
    m_document.remove_line(line_number);
    m_document.set_all_cursors(m_cursor);
}

size_t InsertLineCommand::compute_line_number() const
{
    if (m_pos == InsertPosition::Above)
        return m_cursor.line();

    if (m_pos == InsertPosition::Below)
        return m_cursor.line() + 1;

    VERIFY_NOT_REACHED();
}

DeprecatedString InsertLineCommand::action_text() const
{
    StringBuilder action_text_builder;
    action_text_builder.append("Insert Line"sv);

    if (m_pos == InsertPosition::Above) {
        action_text_builder.append(" (Above)"sv);
    } else if (m_pos == InsertPosition::Below) {
        action_text_builder.append(" (Below)"sv);
    } else {
        VERIFY_NOT_REACHED();
    }

    return action_text_builder.to_deprecated_string();
}

ReplaceAllTextCommand::ReplaceAllTextCommand(GUI::TextDocument& document, DeprecatedString const& text, GUI::TextRange const& range, DeprecatedString const& action_text)
    : TextDocumentUndoCommand(document)
    , m_original_text(document.text())
    , m_new_text(text)
    , m_range(range)
    , m_action_text(action_text)
{
}

void ReplaceAllTextCommand::redo()
{
    m_document.remove(m_range);
    m_document.set_all_cursors(m_range.start());
    auto new_cursor = m_document.insert_at(m_range.start(), m_new_text, m_client);
    m_range.set_end(new_cursor);
    m_document.set_all_cursors(new_cursor);
}

void ReplaceAllTextCommand::undo()
{
    m_document.remove(m_range);
    m_document.set_all_cursors(m_range.start());
    auto new_cursor = m_document.insert_at(m_range.start(), m_original_text, m_client);
    m_range.set_end(new_cursor);
    m_document.set_all_cursors(new_cursor);
}

bool ReplaceAllTextCommand::merge_with(GUI::Command const&)
{
    return false;
}

DeprecatedString ReplaceAllTextCommand::action_text() const
{
    return m_action_text;
}

IndentSelection::IndentSelection(TextDocument& document, size_t tab_width, TextRange const& range)
    : TextDocumentUndoCommand(document)
    , m_tab_width(tab_width)
    , m_range(range)
{
}

void IndentSelection::redo()
{
    auto const tab = DeprecatedString::repeated(' ', m_tab_width);

    for (size_t i = m_range.start().line(); i <= m_range.end().line(); i++) {
        m_document.insert_at({ i, 0 }, tab, m_client);
    }

    m_document.set_all_cursors(m_range.start());
}

void IndentSelection::undo()
{
    for (size_t i = m_range.start().line(); i <= m_range.end().line(); i++) {
        m_document.remove({ { i, 0 }, { i, m_tab_width } });
    }

    m_document.set_all_cursors(m_range.start());
}

UnindentSelection::UnindentSelection(TextDocument& document, size_t tab_width, TextRange const& range)
    : TextDocumentUndoCommand(document)
    , m_tab_width(tab_width)
    , m_range(range)
{
}

void UnindentSelection::redo()
{
    for (size_t i = m_range.start().line(); i <= m_range.end().line(); i++) {
        if (m_document.line(i).leading_spaces() >= m_tab_width)
            m_document.remove({ { i, 0 }, { i, m_tab_width } });
        else
            m_document.remove({ { i, 0 }, { i, m_document.line(i).leading_spaces() } });
    }

    m_document.set_all_cursors(m_range.start());
}

void UnindentSelection::undo()
{
    auto const tab = DeprecatedString::repeated(' ', m_tab_width);

    for (size_t i = m_range.start().line(); i <= m_range.end().line(); i++)
        m_document.insert_at({ i, 0 }, tab, m_client);

    m_document.set_all_cursors(m_range.start());
}

CommentSelection::CommentSelection(TextDocument& document, StringView prefix, StringView suffix, TextRange const& range)
    : TextDocumentUndoCommand(document)
    , m_prefix(prefix)
    , m_suffix(suffix)
    , m_range(range)
{
}

void CommentSelection::undo()
{
    for (size_t i = m_range.start().line(); i <= m_range.end().line(); i++) {
        if (m_document.line(i).is_empty())
            continue;
        auto line = m_document.line(i).to_utf8();
        auto prefix_start = line.find(m_prefix).value_or(0);
        m_document.line(i).keep_range(
            m_document,
            prefix_start + m_prefix.length(),
            m_document.line(i).last_non_whitespace_column().value_or(line.length()) - prefix_start - m_prefix.length() - m_suffix.length());
    }
    m_document.set_all_cursors(m_range.start());
}

void CommentSelection::redo()
{
    for (size_t i = m_range.start().line(); i <= m_range.end().line(); i++) {
        if (m_document.line(i).is_empty())
            continue;
        m_document.insert_at({ i, 0 }, m_prefix, m_client);
        for (auto const& b : m_suffix.bytes()) {
            m_document.line(i).append(m_document, b);
        }
    }
    m_document.set_all_cursors(m_range.start());
}

UncommentSelection::UncommentSelection(TextDocument& document, StringView prefix, StringView suffix, TextRange const& range)
    : TextDocumentUndoCommand(document)
    , m_prefix(prefix)
    , m_suffix(suffix)
    , m_range(range)
{
}

void UncommentSelection::undo()
{
    for (size_t i = m_range.start().line(); i <= m_range.end().line(); i++) {
        if (m_document.line(i).is_empty())
            continue;
        m_document.insert_at({ i, 0 }, m_prefix, m_client);
        for (auto const& b : m_suffix.bytes()) {
            m_document.line(i).append(m_document, b);
        }
    }
    m_document.set_all_cursors(m_range.start());
}

void UncommentSelection::redo()
{
    for (size_t i = m_range.start().line(); i <= m_range.end().line(); i++) {
        if (m_document.line(i).is_empty())
            continue;
        auto line = m_document.line(i).to_utf8();
        auto prefix_start = line.find(m_prefix).value_or(0);
        m_document.line(i).keep_range(
            m_document,
            prefix_start + m_prefix.length(),
            m_document.line(i).last_non_whitespace_column().value_or(line.length()) - prefix_start - m_prefix.length() - m_suffix.length());
    }
    m_document.set_all_cursors(m_range.start());
}

TextPosition TextDocument::insert_at(TextPosition const& position, StringView text, Client const* client)
{
    TextPosition cursor = position;
    Utf8View utf8_view(text);
    for (auto code_point : utf8_view)
        cursor = insert_at(cursor, code_point, client);
    return cursor;
}

TextPosition TextDocument::insert_at(TextPosition const& position, u32 code_point, Client const*)
{
    if (code_point == '\n') {
        auto new_line = make<TextDocumentLine>(*this);
        new_line->append(*this, line(position.line()).code_points() + position.column(), line(position.line()).length() - position.column());
        line(position.line()).truncate(*this, position.column());
        insert_line(position.line() + 1, move(new_line));
        notify_did_change();
        return { position.line() + 1, 0 };
    } else {
        line(position.line()).insert(*this, position.column(), code_point);
        notify_did_change();
        return { position.line(), position.column() + 1 };
    }
}

void TextDocument::remove(TextRange const& unnormalized_range)
{
    if (!unnormalized_range.is_valid())
        return;

    auto range = unnormalized_range.normalized();

    // First delete all the lines in between the first and last one.
    for (size_t i = range.start().line() + 1; i < range.end().line();) {
        remove_line(i);
        range.end().set_line(range.end().line() - 1);
    }

    if (range.start().line() == range.end().line()) {
        // Delete within same line.
        auto& line = this->line(range.start().line());
        if (line.length() == 0)
            return;

        bool whole_line_is_selected = range.start().column() == 0 && range.end().column() == line.length();

        if (whole_line_is_selected) {
            line.clear(*this);
        } else {
            line.remove_range(*this, range.start().column(), range.end().column() - range.start().column());
        }
    } else {
        // Delete across a newline, merging lines.
        VERIFY(range.start().line() == range.end().line() - 1);

        auto& first_line = line(range.start().line());
        auto& second_line = line(range.end().line());

        Vector<u32> code_points;
        code_points.append(first_line.code_points(), range.start().column());
        if (!second_line.is_empty())
            code_points.append(second_line.code_points() + range.end().column(), second_line.length() - range.end().column());
        first_line.set_text(*this, move(code_points));

        remove_line(range.end().line());
    }

    if (lines().is_empty()) {
        append_line(make<TextDocumentLine>(*this));
    }

    notify_did_change();
}

bool TextDocument::is_empty() const
{
    return line_count() == 1 && line(0).is_empty();
}

TextRange TextDocument::range_for_entire_line(size_t line_index) const
{
    if (line_index >= line_count())
        return {};
    return { { line_index, 0 }, { line_index, line(line_index).length() } };
}

TextDocumentSpan const* TextDocument::span_at(TextPosition const& position) const
{
    for (auto& span : m_spans) {
        if (span.range.contains(position))
            return &span;
    }
    return nullptr;
}

void TextDocument::set_unmodified()
{
    m_undo_stack.set_current_unmodified();
}

void TextDocument::set_spans(u32 span_collection_index, Vector<TextDocumentSpan> spans)
{
    m_span_collections.set(span_collection_index, move(spans));
    merge_span_collections();
}

struct SpanAndCollectionIndex {
    TextDocumentSpan span;
    u32 collection_index { 0 };
};

void TextDocument::merge_span_collections()
{
    Vector<SpanAndCollectionIndex> sorted_spans;
    auto collection_indices = m_span_collections.keys();
    quick_sort(collection_indices);

    for (auto collection_index : collection_indices) {
        auto spans = m_span_collections.get(collection_index).value();
        for (auto span : spans) {
            sorted_spans.append({ move(span), collection_index });
        }
    }

    quick_sort(sorted_spans, [](SpanAndCollectionIndex const& a, SpanAndCollectionIndex const& b) {
        if (a.span.range.start() == b.span.range.start()) {
            return a.collection_index < b.collection_index;
        }
        return a.span.range.start() < b.span.range.start();
    });

    // The end of the TextRanges of spans are non-inclusive, i.e span range = [X,y).
    // This transforms the span's range to be inclusive, i.e [X,Y].
    auto adjust_end = [](GUI::TextDocumentSpan span) -> GUI::TextDocumentSpan {
        span.range.set_end({ span.range.end().line(), span.range.end().column() == 0 ? 0 : span.range.end().column() - 1 });
        return span;
    };

    Vector<SpanAndCollectionIndex> merged_spans;
    for (auto& span_and_collection_index : sorted_spans) {
        if (merged_spans.is_empty()) {
            merged_spans.append(span_and_collection_index);
            continue;
        }

        auto const& span = span_and_collection_index.span;
        auto last_span_and_collection_index = merged_spans.last();
        auto const& last_span = last_span_and_collection_index.span;

        if (adjust_end(span).range.start() > adjust_end(last_span).range.end()) {
            // Current span does not intersect with previous one, can simply append to merged list.
            merged_spans.append(span_and_collection_index);
            continue;
        }
        merged_spans.take_last();

        if (span.range.start() > last_span.range.start()) {
            SpanAndCollectionIndex first_part = last_span_and_collection_index;
            first_part.span.range.set_end(span.range.start());
            merged_spans.append(move(first_part));
        }

        SpanAndCollectionIndex merged_span;
        merged_span.collection_index = span_and_collection_index.collection_index;
        merged_span.span.range = { span.range.start(), min(span.range.end(), last_span.range.end()) };
        merged_span.span.is_skippable = span.is_skippable | last_span.is_skippable;
        merged_span.span.data = span.data ? span.data : last_span.data;
        merged_span.span.attributes.color = span_and_collection_index.collection_index > last_span_and_collection_index.collection_index ? span.attributes.color : last_span.attributes.color;
        merged_span.span.attributes.bold = span.attributes.bold | last_span.attributes.bold;
        merged_span.span.attributes.background_color = span.attributes.background_color.has_value() ? span.attributes.background_color.value() : last_span.attributes.background_color;
        merged_span.span.attributes.underline_color = span.attributes.underline_color.has_value() ? span.attributes.underline_color.value() : last_span.attributes.underline_color;
        merged_span.span.attributes.underline_style = span.attributes.underline_style.has_value() ? span.attributes.underline_style : last_span.attributes.underline_style;
        merged_spans.append(move(merged_span));

        if (span.range.end() == last_span.range.end())
            continue;

        if (span.range.end() > last_span.range.end()) {
            SpanAndCollectionIndex last_part = span_and_collection_index;
            last_part.span.range.set_start(last_span.range.end());
            merged_spans.append(move(last_part));
            continue;
        }

        SpanAndCollectionIndex last_part = last_span_and_collection_index;
        last_part.span.range.set_start(span.range.end());
        merged_spans.append(move(last_part));
    }

    m_spans.clear();
    TextDocumentSpan previous_span { .range = { TextPosition(0, 0), TextPosition(0, 0) }, .attributes = {} };
    for (auto span : merged_spans) {
        // Validate spans
        if (!span.span.range.is_valid()) {
            dbgln_if(TEXTEDITOR_DEBUG, "Invalid span {} => ignoring", span.span.range);
            continue;
        }
        if (span.span.range.end() < span.span.range.start()) {
            dbgln_if(TEXTEDITOR_DEBUG, "Span {} has negative length => ignoring", span.span.range);
            continue;
        }
        if (span.span.range.end() < previous_span.range.start()) {
            dbgln_if(TEXTEDITOR_DEBUG, "Spans not sorted (Span {} ends before previous span {}) => ignoring", span.span.range, previous_span.range);
            continue;
        }
        if (span.span.range.start() < previous_span.range.end()) {
            dbgln_if(TEXTEDITOR_DEBUG, "Span {} overlaps previous span {} => ignoring", span.span.range, previous_span.range);
            continue;
        }

        previous_span = span.span;
        m_spans.append(move(span.span));
    }
}

void TextDocument::set_folding_regions(Vector<TextDocumentFoldingRegion> folding_regions)
{
    // Remove any regions that don't span at least 3 lines.
    // Currently, we can't do anything useful with them, and our implementation gets very confused by
    // single-line regions, so drop them.
    folding_regions.remove_all_matching([](TextDocumentFoldingRegion const& region) {
        return region.range.line_count() < 3;
    });

    quick_sort(folding_regions, [](TextDocumentFoldingRegion const& a, TextDocumentFoldingRegion const& b) {
        return a.range.start() < b.range.start();
    });

    for (auto& folding_region : folding_regions) {
        folding_region.line_ptr = &line(folding_region.range.start().line());

        // Map the new folding region to an old one, to preserve which regions were folded.
        // FIXME: This is O(n*n).
        for (auto const& existing_folding_region : m_folding_regions) {
            // We treat two folding regions as the same if they start on the same TextDocumentLine,
            // and have the same line count. The actual line *numbers* might change, but the pointer
            // and count should not.
            if (existing_folding_region.line_ptr
                && existing_folding_region.line_ptr == folding_region.line_ptr
                && existing_folding_region.range.line_count() == folding_region.range.line_count()) {
                folding_region.is_folded = existing_folding_region.is_folded;
                break;
            }
        }
    }

    // FIXME: Remove any regions that partially overlap another region, since these are invalid.

    m_folding_regions = move(folding_regions);

    if constexpr (TEXTEDITOR_DEBUG) {
        dbgln("TextDocument got {} fold regions:", m_folding_regions.size());
        for (auto const& item : m_folding_regions) {
            dbgln("- {} (ptr: {:p}, folded: {})", item.range, item.line_ptr, item.is_folded);
        }
    }
}

Optional<TextDocumentFoldingRegion&> TextDocument::folding_region_starting_on_line(size_t line)
{
    return m_folding_regions.first_matching([line](auto& region) {
        return region.range.start().line() == line;
    });
}

bool TextDocument::line_is_visible(size_t line) const
{
    // FIXME: line_is_visible() gets called a lot.
    //        We could avoid a lot of repeated work if we saved this state on the TextDocumentLine.
    return !any_of(m_folding_regions, [line](auto& region) {
        return region.is_folded
            && line > region.range.start().line()
            && line < region.range.end().line();
    });
}

Vector<TextDocumentFoldingRegion const&> TextDocument::currently_folded_regions() const
{
    Vector<TextDocumentFoldingRegion const&> folded_regions;

    for (auto& region : m_folding_regions) {
        if (region.is_folded) {
            // Only add this region if it's not contained within a previous folded region.
            // Because regions are sorted by their start position, and regions cannot partially overlap,
            // we can just see if it starts inside the last region we appended.
            if (!folded_regions.is_empty() && folded_regions.last().range.contains(region.range.start()))
                continue;

            folded_regions.append(region);
        }
    }

    return folded_regions;
}

}
