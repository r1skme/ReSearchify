# ReSearchify

**Локальный поиск по исходникам на C++20. Без зависимостей во время работы.**

[English](README.md) · [Команды](#команды) · [Для ИИ-агентов](#для-ии-агентов) · [Сборка](#сборка)

ReSearchify индексирует код на твоём ПК. Ищет по словам, точному тексту и регулярным выражениям. Индекс сохраняется, при обновлении повторно обрабатываются изменённые файлы.

> **Статус:** сборка из исходников. Windows/MSVC CI подготовлен, но готовый Windows EXE здесь не проверен и не приложен.

## Быстрый старт

После сборки `rs` открой терминал в папке проекта:

```powershell
rs -i .
rs -s RenderFrame
rs -rs "Render[A-Za-z_]*Frame"
rs -l "std::vector<int>"
rs -u
```

На Windows запускай `rs.exe` из папки сборки или добавь её в `PATH`.

## Команды

| Команда | Что делает |
| --- | --- |
| `rs -i .` | Создать индекс проекта |
| `rs -s "foo AND NOT bar"` | Поиск по токенам |
| `rs -rs "foo[0-9]+"` | Поиск по regex |
| `rs -l "a->b"` | Точное совпадение текста |
| `rs -f "*.cpp"` | Список файлов |
| `rs -r src/main.cpp --lines 10:35` | Прочитать строки из индекса |
| `rs -u` | Обновить индекс |
| `rs -st` | Статистика |
| `rs --help` | Полная справка |

Длинные команды `rs init`, `rs find`, `rs read` и остальные тоже работают. Поддерживаются `AND`, `OR`, `NOT`, скобки и фразы в кавычках. Регистр учитывается. По умолчанию индекс хранится в `.researchify/`.

## Сборка

**Windows 11 — Visual Studio 2022 / MSVC** с C++ и CMake:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
.\build\Release\rs.exe --help
```

**Linux / macOS:**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j 8
ctest --test-dir build --output-on-failure
./build/rs --help
```

Нужен CMake 3.20+ (для генератора Visual Studio — 3.21+) и компилятор с поддержкой C++20. Сторонних библиотек нет.

## Для ИИ-агентов

Прочитай [`AGENTS.md`](AGENTS.md) и [`SKILL.md`](skills/researchify/SKILL.md).

```powershell
rs capabilities --json
rs schema find --json
rs -s RenderFrame --json --limit 8 --max-output-bytes 16384
rs serve --stdio --root .
rs mcp --stdio --root .
```

Для скриптов есть JSON, для серии запросов — JSONL-сессия `serve`, для совместимых агентов — MCP через stdio. Подробнее: [Agent API](docs/AGENT_API.md).

## Важно

- В строгом режиме обновление проверяет содержимое всех доступных файлов. `--mode metadata` быстрее, но может пропустить изменение при прежних размере и времени.
- `compact` экономит место; `fast` добавляет триграммы для ускорения части regex и поиска фраз.
- В индексе хранится исходный текст. Не загружай `.researchify/` в Git и исключай секреты.
- Индексы v1 и v2 несовместимы. Создавай новый каталог индекса.
- Защита от аварийного завершения не гарантирует сохранность при отключении питания.

[Синтаксис поиска](docs/QUERY.md) · [Архитектура](docs/ARCHITECTURE.md) · [Формат индекса](docs/FORMAT_STORAGE.md)

**Лицензия:** пока не выбрана; права на код сохраняются за автором.
