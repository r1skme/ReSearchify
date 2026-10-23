# ReSearchify на Windows / Windows installation

[Все системы / All systems](INSTALL.md) · [macOS](INSTALL_MACOS.md) · [Linux](INSTALL_LINUX.md) · [Для агента / For agents](AGENT_SETUP.md)

## Русский

Готовый `rs.exe` — это **консольная программа**. Не нужно устанавливать Python, CMake, Visual Studio или GitHub CLI. Описанный релиз **проверен на Windows x64**; отдельного нативного релиза для Windows ARM64/32-bit нет.

### 1. Скачай архив и контрольную сумму

Открой [ReSearchify → Releases](https://github.com/r1skme/ReSearchify/releases), последний завершённый preview-релиз, раздел **Assets**. Скачай **оба** файла:

- `ReSearchify-Windows-x64.zip`
- `SHA256SUMS.txt` (именно из того же релиза)

Это не кнопка **Code → Download ZIP**: она скачивает исходный код, не готовую программу.

### 2. Проверь SHA-256 (PowerShell)

Открой **PowerShell**, скопируй весь блок. По умолчанию файлы находятся в «Загрузках» (`Downloads`); если браузер сохранил их в другое место, исправь пути:

```powershell
$zip = Join-Path $HOME 'Downloads\ReSearchify-Windows-x64.zip'
$sums = Join-Path $HOME 'Downloads\SHA256SUMS.txt'
$sumLines = @(Get-Content -LiteralPath $sums | Where-Object { $_ -match '^[0-9a-fA-F]{64}\s+\*?ReSearchify-Windows-x64\.zip$' })
if ($sumLines.Count -ne 1) { throw 'SHA256SUMS.txt does not contain exactly one Windows x64 checksum.' }
$expected = ($sumLines[0] -split '\s+')[0]
$actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
if ($actual -ne $expected) { throw 'SHA-256 mismatch. Stop: do not run this archive.' }
Write-Host 'SHA-256 OK'
```

Продолжай **только** при сообщении `SHA-256 OK`. Если PowerShell сообщил об ошибке, сначала исправь её: не пропускай проверку.

### 3. Распакуй и запусти

В том же PowerShell:

```powershell
$installDir = Join-Path $env:LOCALAPPDATA 'Programs\ReSearchify'
Expand-Archive -LiteralPath $zip -DestinationPath $installDir -Force
$rs = Join-Path $installDir 'rs.exe'
& $rs --version
& $rs --help
```

Это установка в **папку твоего пользователя**, права администратора обычно не нужны. Файл должен лежать прямо в `$installDir\rs.exe`, а не в дополнительной вложенной папке.

`--version` выдаёт **JSON с версией и capabilities**, а `--help` — обычную текстовую справку. Это нормальная работа, отдельного окна ReSearchify нет. Если Windows блокирует неизвестный исполняемый файл, не отключай защиту всей системы: проверь источник, SHA-256 и осознанно реши, доверяешь ли файлу.

### 4. Протестируй в безопасной отдельной папке

Команды создают **один файл** в новой временной папке. Они **не индексируют Downloads**, даже если PowerShell открыт в загрузках:

```powershell
$demo = Join-Path $env:TEMP ('ReSearchify-demo-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $demo | Out-Null
Set-Content -LiteralPath (Join-Path $demo 'example.cpp') -Value 'int RenderFrame() { return 42; }' -Encoding Ascii
& $rs init $demo
& $rs status --root $demo
& $rs find RenderFrame --root $demo
& $rs verify --root $demo
```

Ожидается `files=1`, результат с `example.cpp` и успешный `verify`. Команда `$demo` покажет путь временной папки; позже её можно удалить через Проводник, если не нужна.

### 5. Перейди к своему проекту

Замени `Projects\MyProject` на **настоящий путь к папке с исходным кодом**, в которой нет секретов, не предназначенных для индексации. Кавычки в PowerShell и `-LiteralPath` позволяют использовать пробелы и кириллицу. Эта проверка не даст случайно индексировать текущую папку PowerShell:

```powershell
$project = Join-Path $HOME 'Projects\MyProject'
if (Test-Path -LiteralPath $project -PathType Container) {
    & $rs init $project
    & $rs status --root $project
    & $rs find main --root $project
} else {
    Write-Warning 'Project folder not found. Set $project to your existing source directory.'
}
```

Убедись, что `root=` в статистике указывает на правильный проект. Если `main` отсутствует в исходниках, ищи имя, которое там точно есть. После изменений в коде: `& $rs update --root $project`.

Чтобы писать коротко `rs` в **текущем окне** PowerShell, выполни `$env:Path += ";$installDir"`. Полный путь `& $rs` надёжнее и не требует изменения PATH.

**Дополнительно для разработчиков:** скрипт `scripts/install.ps1` автоматизирует загрузку/проверку, но требует локально скачанного (клонированного) репозитория, установленного GitHub CLI `gh` и доступности GitHub. Для обычной установки всё это не нужно.

### Ошибки

- `rs.exe is not recognized`: запускай `& $rs` из PowerShell; `rs.exe` не добавлен в PATH автоматически.
- Файл `rs.exe` не найден: проверь, куда распаковался ZIP, и не появилась ли лишняя вложенная папка.
- `E_INDEX_NOT_FOUND`: передай `--root $project` либо сначала создай индекс **одобренного** проекта.
- Предупреждения `binary NUL byte` / `invalid UTF-8`: бинарные файлы пропущены. Если их много, проверь, что выбран исходный код, а не все загрузки.
- `rs -i .` нельзя бездумно запускать из `Downloads`. Подробнее: [об ошибочном индексе Downloads](INSTALL.md#я-случайно-проиндексировал-downloads--i-accidentally-indexed-downloads).

## English

ReSearchify is a **PowerShell/terminal CLI**, not a GUI application. The portable Windows release targets **x64** (not native Windows ARM64/32-bit). No compiler, Python, Node.js, or GitHub account is needed.

1. Download `ReSearchify-Windows-x64.zip` and `SHA256SUMS.txt` from the **same completed preview release** under [Assets](https://github.com/r1skme/ReSearchify/releases). **Code → Download ZIP** gives you source, not `rs.exe`.
2. In PowerShell, verify the exact checksum. Edit paths if your browser used another download folder:

```powershell
$zip = Join-Path $HOME 'Downloads\ReSearchify-Windows-x64.zip'
$sums = Join-Path $HOME 'Downloads\SHA256SUMS.txt'
$sumLines = @(Get-Content -LiteralPath $sums | Where-Object { $_ -match '^[0-9a-fA-F]{64}\s+\*?ReSearchify-Windows-x64\.zip$' })
if ($sumLines.Count -ne 1) { throw 'Missing or duplicate Windows checksum.' }
$expected = ($sumLines[0] -split '\s+')[0]
$actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
if ($actual -ne $expected) { throw 'SHA-256 mismatch. Do not continue.' }
Write-Host 'SHA-256 OK'
```

3. **Only after `SHA-256 OK`**, unpack to your own user profile and verify the executable:

```powershell
$installDir = Join-Path $env:LOCALAPPDATA 'Programs\ReSearchify'
Expand-Archive -LiteralPath $zip -DestinationPath $installDir -Force
$rs = Join-Path $installDir 'rs.exe'
& $rs --version
& $rs --help
```

`--version` displays JSON capabilities; `--help` shows human-readable help. No administrator rights or separate desktop window are normally needed.

4. Smoke-test **only a fresh temporary folder** (it works regardless of your current directory):

```powershell
$demo = Join-Path $env:TEMP ('ReSearchify-demo-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $demo | Out-Null
Set-Content -LiteralPath (Join-Path $demo 'example.cpp') -Value 'int RenderFrame() { return 42; }' -Encoding Ascii
& $rs init $demo
& $rs status --root $demo
& $rs find RenderFrame --root $demo
& $rs verify --root $demo
```

You should see `files=1`, `example.cpp` in results and a successful verification. `$demo` prints the demo path; clean it up later using File Explorer if desired.

5. Select a **real existing source directory** and check its root before you index:

```powershell
$project = Join-Path $HOME 'Projects\MyProject'
if (Test-Path -LiteralPath $project -PathType Container) {
    & $rs init $project
    & $rs status --root $project
    & $rs find main --root $project
} else {
    Write-Warning 'Replace $project with an existing source directory.'
}
```

Check `root=` carefully. Replace `main` with an identifier known to exist. After code edits: `& $rs update --root $project`. `& $rs` uses the full executable path and works without PATH changes; optionally use `$env:Path += ";$installDir"` for this session.

Don't index all of Downloads or folders with secrets. If you accidentally indexed Downloads, see the [safe cleanup instructions](INSTALL.md#я-случайно-проиндексировал-downloads--i-accidentally-indexed-downloads). The optional `scripts/install.ps1` is for users who already have this repository locally and have GitHub CLI installed; it is not required for the manual path above.
