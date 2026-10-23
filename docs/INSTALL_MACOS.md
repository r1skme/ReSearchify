# ReSearchify на macOS / macOS installation

[Все системы / All systems](INSTALL.md) · [Windows](INSTALL_WINDOWS.md) · [Linux](INSTALL_LINUX.md) · [Для агента / For agents](AGENT_SETUP.md)

## Русский

Это **консольная утилита**. Для запуска готового архива не нужно устанавливать Python, CMake или Xcode. Совместимая версия: **macOS 13.3+**. Сборки пока не подписаны и не нотариализованы Apple.

### 1. Выбери и скачай два файла

1. Открой [ReSearchify → Releases](https://github.com/r1skme/ReSearchify/releases) и выбери последний **завершённый** preview-релиз (не исходники через кнопку Code).
2. Открой **Терминал** и выполни `uname -m`.
3. Если результат `arm64` (Mac на M1/M2/M3/M4/M5…), скачай `ReSearchify-macOS-arm64.tar.gz`. Если `x86_64` (Intel Mac), скачай `ReSearchify-macOS-x64.tar.gz`.
4. Скачай **`SHA256SUMS.txt` из того же релиза**. Оба файла должны лежать в `~/Downloads`. Если браузер уже распаковал архив и удалил `.tar.gz`, скачай архив заново без автоматической распаковки.

### 2. Проверь скачанный архив

Выполни `cd "$HOME/Downloads"`. Выбери **одну** команду — под свой процессор:

**Apple Silicon:**

```sh
grep -F 'ReSearchify-macOS-arm64.tar.gz' SHA256SUMS.txt | shasum -a 256 -c -
```

**Intel:**

```sh
grep -F 'ReSearchify-macOS-x64.tar.gz' SHA256SUMS.txt | shasum -a 256 -c -
```

Нужен вывод вида `ReSearchify-macOS-arm64.tar.gz: OK` (или `x64: OK`). Если `FAILED`, `No such file` или ничего не получилось проверить — **остановись** и проверь имя файла/релиз. Контрольная сумма проверяет соответствие скачанного архива файлу сумм этого релиза.

### 3. Распакуй и проверь запуск

Команды ниже выполняются из `~/Downloads` после успешного SHA-256.

**Apple Silicon:**

```sh
mkdir -p "$HOME/Tools/ReSearchify"
tar -xzf ReSearchify-macOS-arm64.tar.gz -C "$HOME/Tools/ReSearchify"
"$HOME/Tools/ReSearchify/rs" --version
"$HOME/Tools/ReSearchify/rs" --help
```

**Intel:** та же установка, только архив другой:

```sh
mkdir -p "$HOME/Tools/ReSearchify"
tar -xzf ReSearchify-macOS-x64.tar.gz -C "$HOME/Tools/ReSearchify"
"$HOME/Tools/ReSearchify/rs" --version
"$HOME/Tools/ReSearchify/rs" --help
```

`--version` выводит **JSON** с `"version":"2.0.0"` и возможностями — это нормально. `--help` выводит обычную текстовую справку. Если файл `rs` находится в `~/Tools/ReSearchify`, установка прошла. Никакое окно при двойном нажатии на `rs` не требуется.

**macOS заблокировала запуск?** Программа пока не нотариализована. Не отключай Gatekeeper целиком и не запускай произвольные команды удаления карантина. Убедись в происхождении и SHA-256 архива; только если сознательно доверяешь этому бинарнику, после попытки запуска зайди в **Системные настройки → Конфиденциальность и безопасность** и найди **«Всё равно открыть»**. Если такого варианта нет, остановись и изучи [инструкцию Apple](https://support.apple.com/ru-ru/102445).

### 4. Безопасный тест в отдельной папке

**Не запускай `rs -i .` в `Downloads`!** Точка (`.`) означает текущую папку. Чтобы проверить программу, создай маленький временный проект. Этот блок не трогает твои существующие проекты:

```sh
demo="$(mktemp -d)"
printf 'int RenderFrame() { return 42; }\n' > "$demo/example.cpp"
"$HOME/Tools/ReSearchify/rs" init "$demo"
"$HOME/Tools/ReSearchify/rs" status --root "$demo"
"$HOME/Tools/ReSearchify/rs" find RenderFrame --root "$demo"
"$HOME/Tools/ReSearchify/rs" verify --root "$demo"
```

Ожидается `files=1`, файл `example.cpp` в результатах поиска и успешная проверка `verify`. Индекс находится в `"$demo/.researchify"`; `echo "$demo"` покажет путь. Это тестовые данные в системной временной папке, их можно удалить позже через Finder после проверки пути.

### 5. Индексируй настоящий проект — строго по его пути

Вместо `$HOME/Projects/MyProject` укажи **существующую** папку с исходниками. Пробелы/русские символы в пути допустимы — сохраняй кавычки. Чтобы получить путь, можно перетащить папку из Finder в терминал после ввода `PROJECT=`.

```sh
PROJECT="$HOME/Projects/MyProject"
if [ -d "$PROJECT" ]; then
  "$HOME/Tools/ReSearchify/rs" init "$PROJECT"
  "$HOME/Tools/ReSearchify/rs" status --root "$PROJECT"
  "$HOME/Tools/ReSearchify/rs" find main --root "$PROJECT"
else
  echo "Project folder not found. Edit PROJECT to an existing source-code folder."
fi
```

Проверь строку `root=` в `status`: она должна указывать именно на этот проект. Ищи **существующее в твоём коде** имя вместо `main`. После правки файлов: `"$HOME/Tools/ReSearchify/rs" update --root "$PROJECT"`.

Для короткой команды `rs` только **в текущем терминале** можешь выполнить `export PATH="$HOME/Tools/ReSearchify:$PATH"`. Для постоянного PATH добавь такую строку в конфигурацию своей оболочки вручную; это необязательно.

### Если выводит много предупреждений

`warning: ... binary NUL byte`, `invalid UTF-8`, `file exceeds max-file limit` означают, что файлы пропущены. Если там `.zip`, `.png`, `.mp4` и т. п., ты, скорее всего, выбрал не каталог проекта. `init` хранит копию индексируемого текста в `.researchify/`, поэтому не выбирай папки с секретами/личными данными. Если по ошибке проиндексировал Downloads, см. [как удалить только ошибочный индекс](INSTALL.md#я-случайно-проиндексировал-downloads--i-accidentally-indexed-downloads).

## English

ReSearchify is a **terminal CLI**, not a GUI application. Prebuilt executables need no Python, CMake, or Xcode. macOS **13.3+** is targeted; binaries are **not Apple-signed/notarized**.

1. Run `uname -m` in Terminal. Download `ReSearchify-macOS-arm64.tar.gz` for `arm64` (Apple Silicon) or `ReSearchify-macOS-x64.tar.gz` for `x86_64` (Intel) from the newest completed [preview release](https://github.com/r1skme/ReSearchify/releases). Download `SHA256SUMS.txt` **from the same release**; keep both files in Downloads.
2. Run `cd "$HOME/Downloads"`, then check the exact archive:
   - Apple Silicon: `grep -F 'ReSearchify-macOS-arm64.tar.gz' SHA256SUMS.txt | shasum -a 256 -c -`
   - Intel: `grep -F 'ReSearchify-macOS-x64.tar.gz' SHA256SUMS.txt | shasum -a 256 -c -`
   Continue **only** after the result says `OK`.
3. Extract the matching file to `$HOME/Tools/ReSearchify`, then launch `rs` by its full path. For Apple Silicon (replace `arm64` with `x64` for Intel):

```sh
mkdir -p "$HOME/Tools/ReSearchify"
tar -xzf ReSearchify-macOS-arm64.tar.gz -C "$HOME/Tools/ReSearchify"
"$HOME/Tools/ReSearchify/rs" --version
"$HOME/Tools/ReSearchify/rs" --help
```

`--version` returning JSON capabilities is expected. If Gatekeeper blocks execution, review the source and hash; do **not** disable system security globally. Apple's [official instructions](https://support.apple.com/en-us/102445) explain the opt-in **System Settings → Privacy & Security → Open Anyway** route after a blocked attempt.

4. Run the isolated demo below. It indexes **only a fresh temporary folder**, even if Terminal is currently in Downloads.

```sh
demo="$(mktemp -d)"
printf 'int RenderFrame() { return 42; }\n' > "$demo/example.cpp"
"$HOME/Tools/ReSearchify/rs" init "$demo"
"$HOME/Tools/ReSearchify/rs" status --root "$demo"
"$HOME/Tools/ReSearchify/rs" find RenderFrame --root "$demo"
"$HOME/Tools/ReSearchify/rs" verify --root "$demo"
```

Expected: `files=1`, `example.cpp` in results, verification success. `echo "$demo"` prints the disposable test directory.

5. For a real project, set `PROJECT` to the full path of an existing source directory. This guards against accidentally indexing Downloads or a nonexistent placeholder:

```sh
PROJECT="$HOME/Projects/MyProject"
if [ -d "$PROJECT" ]; then
  "$HOME/Tools/ReSearchify/rs" init "$PROJECT"
  "$HOME/Tools/ReSearchify/rs" status --root "$PROJECT"
  "$HOME/Tools/ReSearchify/rs" find main --root "$PROJECT"
else
  echo "Project folder not found: edit PROJECT first."
fi
```

Confirm `root=` matches the intended project; use an identifier actually present in that project. Refresh after source changes with `"$HOME/Tools/ReSearchify/rs" update --root "$PROJECT"`. Optionally run `export PATH="$HOME/Tools/ReSearchify:$PATH"` to use `rs` without its full path in the **current** shell.

Warnings about binary data, invalid UTF-8 or oversized files mean skipped inputs; a long list of media/archive filenames is a hint that the index root is too broad. Keep `.researchify/` out of version control and avoid indexing confidential material. For accidental Downloads indexing, see [cleanup guidance](INSTALL.md#я-случайно-проиндексировал-downloads--i-accidentally-indexed-downloads).
