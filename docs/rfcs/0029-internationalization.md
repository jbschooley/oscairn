# RFC 0029: Internationalization

Status: proposed, 2026-11-01. Required for every product Oscairn runs on. Builds on the design
language's [themes and layouts](../design-language.md), adaptive UI
([RFC 0019](0019-adaptive-ui.md)) and packages ([RFC 0006](0006-packages-and-portability.md)).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. Every user-facing string through a catalog, in American English (the source language)
- [ ] 2. Translations installable as packages, chosen in settings
- [ ] 3. Locale-aware numbers, times, dates and note names
- [ ] 4. Layouts that fit longer text, checked with a pseudo-translation
- [ ] 5. Right-to-left scripts and the fonts for the languages offered
- [ ] 6. Plugins' and apps' own strings, translated the same way

## Problem

Oscairn's text is in the code, in English. Musicians everywhere play, and a product sold in
several countries needs its screens in their languages: the panel's labels, error messages,
the library's words, a plugin editor's parameter names.

## Outcome

Text the user reads comes from a catalog, in American English until translated
(the [roadmap](../roadmap.md) rule: American spelling throughout until now). Translations are
packages anyone can make and install, like themes. Numbers, times and note names follow the
user's locale. Layouts leave room for longer words and right-to-left scripts.

## Catalogs

- Each component (the core's messages, the desktop app, each plugin and app) has its own catalog
  of messages, keyed by an ID, with the English text as the source.
- Messages carry placeholders and plural forms (ICU MessageFormat), never strings joined in code:
  "3 Performances" is one message with a count, not a number and a word.
- Protocol and file formats stay in English: method names, field names and error codes are not
  translated; the error *text* the UI shows is.

## Translations

- A translation is a package (RFC 0006) holding catalogs for the components it covers, with its
  language and the versions it was made for; missing messages fall back to English.
- Chosen in settings; changing it redraws the UI without restarting.

## Locale formatting

Numbers (decimal separators), times and dates, durations, and note names (C D E or Do Re Mi,
H for B in German) follow the locale, with a setting to override note naming apart from
language.

## Layouts

- Layouts and view models (RFC 0019) never fix a label's width to English text; tests render
  every view with a pseudo-translation (longer text, accented letters) and fail on clipped
  labels.
- Right-to-left scripts mirror layouts where direction carries meaning (lists, menus), not where
  it does not (keyboards, timelines, signal flow).
- Fonts: themes name fonts for Latin text today; a theme or the system supplies fallbacks for
  other scripts.

## Open questions

- Whether plugin parameter names are translated (they are part of a plugin's identity in saved
  state, and must stay stable).
- How translations are kept in step with new messages between releases.

## Ruled out

- **Translating identifiers, file formats or the control protocol.**
- **Strings built by joining words in code.**
