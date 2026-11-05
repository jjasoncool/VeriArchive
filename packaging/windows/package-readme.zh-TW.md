# VeriArchive

VeriArchive 為封存用的資料碟留下「完整性證據」，日後可以證明裡面的檔案有沒有被修改、損毀、刪除或多出檔案。它只負責**偵測**：不會修復、不會還原，也不會幫你備份。

[English](README.md)

## 啟動程式

解壓縮 zip 的資料夾（例如 `D:\VeriArchive`）就是整個程式，不需要再安裝其他東西。

- `bin\veriarchive-gui.exe` 是圖形介面：按兩下就會啟動。
- `bin\veriarchive-cli.exe` 是命令列，在 PowerShell 或命令提示字元中使用；不加任何指令執行時，會列出所有指令。
- `bin\veriarchive-device-identity.exe` 是裝置身分讀取程式。只有在您要求讀取外接盒內裝置本身的序號時，視窗才會啟動它，Windows 也會先詢問是否允許它取得系統管理員權限。請不要自行開啟它。

## 使用說明

使用說明就放在這個檔案旁邊，附有圖形介面的截圖：英文版 [user-guide.md](user-guide.md)，繁體中文版 [user-guide.zh-TW.md](user-guide.zh-TW.md)。請從這裡開始：它會帶你從建立儲存庫一路做到驗證封存，以及和副本比較，圖形介面和命令列都有說明。

## 紀錄放在哪裡

VeriArchive 把每個封存的基準（清單檔）和每一次驗證都存放在**儲存庫**。第一次使用時，程式在 `bin\data` 建立儲存庫，並記在 `bin\config.json`；程式資料夾無法寫入時，改用個人資料夾中的 `VeriArchive`，並記在 `%APPDATA%\VeriArchive\config.json`。「設定」頁會顯示目前使用的儲存庫。

**請備份儲存庫。** 遺失它，驗證歷程也就跟著遺失。備份整個程式資料夾，也就一起備份了 `bin\data`。

更新程式時，先關閉程式，再把新版 zip 解壓縮到同一個資料夾：zip 裡沒有 `bin\data` 和 `bin\config.json`，所以你的紀錄會保留下來。做法見使用說明的「安裝（Windows）」。

這個檔案旁邊的 `NOTICE.txt` 列出程式使用的函式庫；`share\licenses` 收錄它們的授權。
