# Install ReSearchify

[Русский](#установка-на-windows) · [English](#install-on-windows) · [Build from source](#build-from-source)

## Установка на Windows

**Скачай архив, распакуй и запускай rs.exe. Компилятор не нужен.**

1. Войди в GitHub под учётной записью с доступом к этому закрытому репозиторию.
2. Перейди на [Releases](https://github.com/r1skme/ReSearchify/releases).
3. Скачай `ReSearchify-Windows-x64.zip` из последнего preview-релиза и распакуй в `C:\Tools\ReSearchify`.
4. Открой PowerShell в папке своего проекта и выполни:

```powershell
$env:Path += ';C:\Tools\ReSearchify'     # Путь только для этого окна
rs --help                                # Проверить запуск
rs -i .                                  # Создать индекс проекта
rs -s "RenderFrame"                      # Найти функцию
rs -u                                    # Обновить индекс после изменений
```

Индекс создаётся в папке `.researchify/` внутри твоего проекта, а сам `rs.exe` может лежать где угодно. Если команда не находится, проверь папку с exe и `$env:Path`.

В Releases пока нет архива? Загляни в [Actions](https://github.com/r1skme/ReSearchify/actions): он появится только после успешной сборки и тестов. Не путай `Code → Download ZIP` (исходный код) с готовой программой.

**Установка в профиль пользователя**, если GitHub CLI уже установлен и авторизован:

```powershell
gh auth status
.\scripts\install.ps1
rs --help
```

Установщик сверяет SHA-256 архива, помещает `rs.exe` в `%LOCALAPPDATA%\Programs\ReSearchify` и добавляет эту папку в пользовательский PATH. Права администратора не требуются. Другая папка: `.\scripts\install.ps1 -InstallDir 'D:\Tools\ReSearchify'`.

## Install on Windows

1. Sign in to GitHub using an account that has access to this private repository.
2. Download `ReSearchify-Windows-x64.zip` from the newest [preview release](https://github.com/r1skme/ReSearchify/releases).
3. Extract to `C:\Tools\ReSearchify`.
4. In PowerShell, from your source-code project folder:

```powershell
$env:Path += ';C:\Tools\ReSearchify'
rs --help
rs -i .
rs -s RenderFrame
rs -u
```

The portable executable requires no Python, Node.js, compiler, or background service. You need GitHub authentication to **download the private release**, not to run the program.

You can also use `.\scripts\install.ps1` from a checkout when GitHub CLI is already authenticated. It checks the release SHA-256 before installing.

## For AI agents

Give the following to an agent that can execute programs on your Windows computer:

> Install ReSearchify for my project. Read README.md, AGENTS.md and skills/researchify/SKILL.md. Download the newest Windows x64 release, verify the SHA-256 against SHA256SUMS.txt, and install rs.exe. Ask me which directory to index before running rs -i. Demonstrate a search and explain how to update. If no release exists, ask before compiling.

A browser-only assistant without access to your computer cannot install the program. A private repo requires existing permission; never paste access tokens into a chat.

## Build from source

Use this if you want to modify the engine or no Windows release is available.

**Windows 11 — Visual Studio 2022 with Desktop development with C++, CMake 3.21+:**

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
.\build\Release\rs.exe --help
```

**Linux / macOS — C++20 compiler, CMake 3.20+:**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure
./build/rs --help
```

## Troubleshooting

- **Command not found:** add the directory containing rs.exe to PATH, or run the absolute path.
- **No index:** run `rs -i .` once in the desired project.
- **No matches:** search is case-sensitive. Try `rs -l "exact text"` or `rs -rs "pattern"`.
- **Code changed:** run `rs -u`. An update with `--trust-mtime` is faster but can miss some changes.
- **Unexpected error:** run `rs verify` to inspect index integrity; do not force recovery blindly.

[README](../README.md) · [README на русском](../README.ru.md).
