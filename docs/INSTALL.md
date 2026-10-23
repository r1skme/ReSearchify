# ReSearchify — установка / Installation

**ReSearchify — консольная программа, а не приложение с окном.** После установки она запускается в PowerShell (Windows) или Terminal (macOS/Linux). Python, Node.js, CMake и аккаунт GitHub для запуска готового архива не нужны.

**ReSearchify is a command-line tool, not a desktop app.** Use PowerShell on Windows or Terminal on macOS/Linux. Prebuilt packages need no Python, Node.js, compiler, or GitHub account.

## Выбери свою систему / Choose your system

| Платформа / Platform | Пошаговая инструкция / Step-by-step guide | Файл в [Releases](https://github.com/r1skme/ReSearchify/releases) |
| --- | --- | --- |
| Windows x64 | [Windows — RU + EN](INSTALL_WINDOWS.md) | `ReSearchify-Windows-x64.zip` |
| Mac с процессором Apple M1/M2/M3/M4… | [macOS — RU + EN](INSTALL_MACOS.md) | `ReSearchify-macOS-arm64.tar.gz` |
| Mac с процессором Intel | [macOS — RU + EN](INSTALL_MACOS.md) | `ReSearchify-macOS-x64.tar.gz` |
| Linux x86-64 | [Linux — RU + EN](INSTALL_LINUX.md) | `ReSearchify-Linux-x64.tar.gz` |
| Linux ARM64 | [Linux — RU + EN](INSTALL_LINUX.md) | `ReSearchify-Linux-arm64.tar.gz` |

Use the latest **completed** preview release with the appropriate asset. Also download `SHA256SUMS.txt` from **the same release** and check the hash before running anything. **Code → Download ZIP** downloads source code, not a ready-to-run binary. A GitHub Actions artifact is not the same as a published, tested release.

> **Главная ошибка при первом запуске / Most common first-run mistake**
>
> `rs -i .` индексирует **текущий каталог терминала**. Если ты открыл терминал в `Downloads`, программа проиндексирует Downloads, включая подходящие текстовые файлы из вложенных папок. Распаковка и установка `rs` **не** означают, что ты перешёл в папку проекта.
>
> `rs -i .` indexes the **current working directory**. If that directory is Downloads, Downloads gets indexed. Installing `rs` does not change your working directory. Prefer `rs init "FULL_PROJECT_PATH"`, with an explicit approved path.

## Правильный первый тест / Safe first test

1. Выполни шаги установки на странице своей ОС и проверь `rs --version` или `rs.exe --version`. Вывод **JSON с `version:"2.0.0"` и capabilities — нормален**. `--help` показывает обычную текстовую справку.
2. Сначала используй **отдельную временную папку с одним тестовым файлом**. Готовые команды для такого теста есть в каждом OS-гайде. Ты должен увидеть `files=1`, один результат по `RenderFrame`, а `verify` должен пройти успешно.
3. Только потом выбери существующую папку **исходного кода** и явно передай её в `init`. Перед индексацией исключи папки с секретами и данными, которые не хочешь сохранять в индексе.

1. Follow the OS guide; `--version` returning JSON capabilities is expected.
2. Start with the OS guide's isolated **one-file demo**, not your Downloads folder.
3. Choose a real source-code project afterward, confirm its full path, and index only that approved root. The index stores a copy of source text.

**Обновление:** `rs update --root "PROJECT_PATH"` обновляет **поисковый индекс**, но **не саму программу**. Чтобы установить новую версию `rs`, скачай новый архив и его SHA256SUMS, проверь и замени исполняемый файл по OS-гайду. Существующий `.researchify/` при обычном обновлении программы не удаляется; если формат индекса несовместим, программа сообщит об этом — потребуется пересоздание индекса с согласия владельца.

**Upgrading:** `rs update --root "PROJECT_PATH"` updates the **index**, not the installed executable. To upgrade `rs`, verify a newer release and replace the binary following the OS guide; retain the existing index unless the new version explicitly reports incompatible format. Don't delete indexes automatically.

При создании индекса рядом с исходниками появляется скрытая папка **`.researchify/`** — это база поиска, а не оригиналы файлов. Добавь её в `.gitignore`. `rs update` обновляет индекс после изменений исходников. `rs status --root "PROJECT_PATH"` показывает **root** и статистику; обязательно проверь, что root совпадает с твоим проектом.

The **`.researchify/`** directory contains the persistent index (including indexed text). Keep it out of Git and backups shared with others when it may contain confidential code. `rs update --root "PROJECT_PATH"` refreshes it after edits; `rs status --root "PROJECT_PATH"` lets you confirm the indexed root.

## Частые вопросы / Frequently asked questions

| Симптом / Symptom | Что делать / What to do |
| --- | --- |
| `command not found` / `rs is not recognized` | Используй полный путь к исполняемому файлу из OS-гайда. Добавление в `PATH` — удобство, а не обязательный шаг. / Use the absolute executable path from the OS guide; PATH is optional. |
| `rs --version` вывел много JSON / printed JSON | Это ожидаемый формат: версия и возможности. / This is expected capability/version output. |
| `binary NUL byte`, `invalid UTF-8`, `file exceeds max-file limit` при `init` | Такие файлы пропускаются (`skipped`); это не означает поломку индекса. Обычно выбрана слишком широкая папка. / Those files are skipped; check that you indexed a source project, not general downloads or media. |
| `No .researchify marker found` | Проект не индексировался или указан неверный root. Проверь путь, затем `init` только с разрешения владельца. / Check the path and seek approval before creating an index. |
| Поиск ничего не нашёл / No matches | Регистр букв важен. Ищи термин, который точно есть в исходниках, или попробуй буквальный поиск `-l`. / Searches are case-sensitive; verify a known source identifier or use `-l`. |
| `E_INDEX_CORRUPT` / ошибка индекса | Проверь `rs verify --root "PROJECT_PATH"`; не запускай `recover --force` и не удаляй индекс без понимания последствий. / Verify and inspect; never force recovery automatically. |

### Я случайно проиндексировал Downloads / I accidentally indexed Downloads

Если `rs status --root "$HOME/Downloads"` (macOS/Linux) или `rs status --root "$HOME\Downloads"` (Windows PowerShell) показывает корнем папку загрузок, это **не затронуло сами загрузки** — но рядом с ними создался индекс `Downloads/.researchify`, который занимает место и может хранить копии текста. После проверки точного пути можешь **переместить именно папку `.researchify` из Downloads в корзину через файловый менеджер**. Не удаляй весь `Downloads` и не трогай индексы других проектов.

If Downloads was indexed by mistake, check the reported root first, then move **only** `Downloads/.researchify` to the Trash/Recycle Bin using your file manager. This removes that index, not the downloaded files. Do not delete the whole Downloads folder.

## Инструкция для ИИ-агента / AI agent setup

См. [AGENT_SETUP.md](AGENT_SETUP.md): определение ОС и архитектуры, проверки контрольных сумм, подтверждение области индексации, безопасный тест, коды ошибок, JSON/JSONL/MCP и проверки свежести. Агенту нужен настоящий доступ к терминалу и разрешение запускать программы; веб-чат без такого доступа не может установить `rs`.

See [AGENT_SETUP.md](AGENT_SETUP.md) for an executable-by-environment checklist and protocol rules. A web-only AI chat cannot install or run a program on your device.

[README EN](../README.md) · [README RU](../README.ru.md) · [Search syntax](QUERY.md) · [Agent API](AGENT_API.md)
