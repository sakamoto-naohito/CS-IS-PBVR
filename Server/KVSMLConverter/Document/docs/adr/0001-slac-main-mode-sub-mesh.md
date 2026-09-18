# SLAC の main/sub ファイル割り当て

- 状態: 採用
- 日付: 2026-09-18

## 背景

KVSML コンバータの NetCDF 入力は、変換対象を main ファイル、読み込みを補助するファイルを sub ファイルとして扱う。従来の SLAC 読み込みだけは mesh ファイルが main、mode ファイルが sub であり、時系列の変換対象が main になる他の処理と役割が一致していなかった。

## 決定

SLAC 読み込みでは、mode ファイルを main ファイル、mesh ファイルを sub ファイルとして指定する。

| 入力の役割 | SLAC ファイル | ワイルドカード |
| --- | --- | --- |
| main | mode | 使用可 |
| sub | mesh | 使用不可 |

`cvt::NetCDF` の `filename` には mode ファイルを、`sub_file_path` には mesh ファイルを渡す。`vtkSLACReader` には従来どおり、それぞれ `AddModeFileName` と `SetMeshFileName` で設定する。

## 影響

- mode ファイルの連番を main ファイルのワイルドカードで指定できる。
- mesh ファイルは固定の sub ファイルとして対話入力する。
- 従来の `main = mesh、sub = mode` という引数の組み合わせには互換性がない。
- CAM の `main = data、sub = connectivity` という割り当ては変更しない。
