# Server 用語集

Server の設定および処理で使用する用語を定義する。

## main ファイル

Server が処理対象として直接受け取る入力ファイル。時系列変換ではワイルドカードを使用できる。

SLAC NetCDF データでは mode ファイルを指す。

## sub ファイル

main ファイルの読み込みを補助する固定ファイル。現在は SLAC NetCDF データと CAM NetCDF データで使用する。

CAM 形式では connectivity ファイル、SLAC 形式では mesh ファイルを指す。
