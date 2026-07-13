# Radic

Qt6 と FreeRDP を使ったシンプルで軽量な Linux 向け RDP（Remote Desktop Protocol）クライアント。

## 基本情報

- **プロジェクト名**: Radic
- **開発者**: soramimi.jp
- **プログラミング言語**: C++17
- **GUIフレームワーク**: Qt 6（ローカルのビルド成果物ディレクトリ名は `Qt_6_8_3`。要求バージョンは特に固定していない）
- **RDPライブラリ**: FreeRDP 3.x
- **ビルドシステム**: qmake
- **対象OS**: Linux（X11/Wayland、Qt が動く環境）

## 依存関係

- Qt 6（core, gui, widgets）
- FreeRDP 3.x
  - libfreerdp3
  - libfreerdp-client3
  - libwinpr3
- インクルードパス: `/usr/include/freerdp3`, `/usr/include/winpr3`

## ビルド

```bash
qmake Radic.pro
make
```

- 出力実行ファイル: `Radic`
- Debug/Release のビルド成果物は `build/Qt_x_y_z-{Debug,Release}/` 配下に生成される

## 設定ファイル

- パス: `~/.config/soramimi.jp/Radic/Radic.ini`（`Global.h` の `ORGANIZATION_NAME`/`APPLICATION_NAME` と `MySettings` で管理）
- 保存項目: ウィンドウジオメトリ、最大化状態
- 接続履歴の保存は未実装（`ConnectionDialog` は前回接続した host/user 等を `MySettings` から復元するのみ）

## ファイル構成

```
main.cpp                  - エントリーポイント（ApplicationGlobal初期化、QApplication起動）
Global.cpp/h              - アプリ全体の組織名/アプリ名/設定パスなどのグローバル定義
MainWindow.cpp/h/ui       - メインウィンドウ、RDP接続制御、FreeRDPコールバック、キーイベントフィルタ
MyView.cpp/h              - リモート画面描画・マウス/キーボード入力処理ウィジェット
ConnectionDialog.cpp/h/ui - 接続情報入力ダイアログ
VerifyCertificateDialog.cpp/h/ui - TLS証明書検証UI
rdpcert.cpp/h             - 証明書情報の構造体・検証結果の定義
CommandForm.cpp/h/ui      - マジックキーで表示切り替えする補助コマンドパネル
MySettings.cpp/h          - QSettingsベースの設定永続化
joinpath.cpp/h            - クロスプラットフォーム対応のパス結合ユーティリティ
Radic.pro                 - qmakeプロジェクトファイル
```

## アーキテクチャ

### MainWindow
メインウィンドウとRDP接続の中心的な管理を行う。

- FreeRDPインスタンス/コンテキストの生成・破棄、各種コールバック実装（pre/post connect, post disconnect, authenticate, verify certificate, end paint, resize display, channel connect/disconnect）
- 別スレッド（`start_rdp_thread`）でFreeRDPのイベントハンドルをポンプし、`freerdp_shall_disconnect_context` による自動切断検知を行う
- 画面更新タイマー（10ms間隔）の管理
- フルスクリーン切り替え、動的解像度（Dynamic Resolution）切り替え
- `eventFilter` によるアプリケーション全体のキーイベント先取り（マジックキー処理、下記参照）
- ウィンドウ状態の永続化（`MySettings` 経由）

#### RdpSession 抽象化（進行中のリファクタリング）
`MainWindow.cpp` 内に、FreeRDPのクライアントAPIを抽象化する `RdpSession` 基底クラスと `RdpSessionV1`/`RdpSessionV2` サブクラスが定義されている（ヘッダには分離されておらず `MainWindow.cpp` の翻訳単位内にのみ存在。`MainWindow.h` には `friend class RdpSessionV2;` の前方宣言のみ）。

- **RdpSessionV1**: レガシーAPI（`freerdp_new()` / `freerdp_context_new()`）を使う実装
- **RdpSessionV2**: 新しいクライアントエントリポイントAPI（`freerdp_client_context_new()` + `RDP_CLIENT_ENTRY_POINTS`）を使う実装。`MyClientContext`（`rdpClientContext` のラッパー）を介してコンテキストを管理

現在 `MainWindow::MainWindow()` では **常に `RdpSessionV2` のみがインスタンス化される**（`m->session = std::make_shared<RdpSessionV2>();`）。コード中には `m->session->version() == RdpSession::V1` を分岐条件にした処理が複数箇所（`start_rdp_thread`、`doConnect`、`channelConnected` 等）に残っているが、V1が生成されることはないため、これらのV1分岐は現状デッドコードになっている。将来的な整理対象として認識しておくこと。

### MyView
リモートデスクトップ画面の表示と入力処理を担当するウィジェット。

- `QImage` によるフレームバッファ描画（`setImage`/`paintEvent`）
- マウス入力（クリック、移動、ホイール）をFreeRDPに転送（`qtToRdpMouseButton` でボタンマッピング）
- キーボード入力をFreeRDPに転送（詳細は下記キーボード仕様を参照）
- 画面スケーリング（1倍/2倍）と座標変換（Qt座標系 ↔ RDP座標系）
- 複合キーシーケンスを順序通り送信するための「キーチャンク」キューイング機構（`addKeyChunk`/`addKey`/`addNativeKey`/`sendKeyChunk`）

### ConnectionDialog
RDP接続情報（ホスト名/IP、ユーザー名、パスワード、ドメイン）の入力UI。デフォルト値としてホスト `192.168.0.20`、ドメイン `WORKGROUP` を持つ。

### VerifyCertificateDialog / rdpcert
TLS証明書検証UI。`rdpcert::Certificate` 構造体と `CertResult`（Reject / AcceptPermanently / AcceptTemporarily）で結果を表現する。

### CommandForm
マジックキー（`Ctrl+Shift+Alt+Backspace`）で表示/非表示を切り替える補助コマンドパネル。

### MySettings
`QSettings` ベースの設定永続化。ウィンドウジオメトリ・最大化状態などをINI形式で保存。

### Global / ApplicationGlobal
組織名・アプリケーション名・設定ファイルパスなどアプリ全体の基本情報を保持するグローバルシングルトン。

### joinpath
クロスプラットフォーム対応のパス結合ユーティリティ。文字列/QString双方に対応し、クォート除去・区切り文字正規化を行う。

## RDP接続・表示機能

- **対応プロトコル**: RDP
- **色深度**: 32bit（画面バッファは `PIXEL_FORMAT_RGB24` / `QImage::Format_RGB888`）
- **認証**: ユーザー名/パスワード認証、Windowsドメイン対応
- **解像度**: デフォルト1920x1080。**動的解像度切り替え機能が実装済み**（View メニューの「Dynamic Resolution」トグル、および `Ctrl+Shift+Alt+D` のスケール切り替え時に連動して `resizeDynamicLater()`/`resizeDynamic()` が呼ばれる。単純な固定解像度ではない点に注意）
- **画面更新頻度**: 10ms間隔のタイマー（`m->update_timer.setInterval(10)`）
- **パフォーマンス最適化**: FastPath、ビットマップキャッシュ、RDP8圧縮、オフスクリーンサポート、グリフサポート、サーフェスコマンド、ネットワーク自動検出を有効化

### マウス操作
- 左クリック: `PTR_FLAGS_BUTTON1`
- 右クリック: `PTR_FLAGS_BUTTON2`
- 中クリック: `PTR_FLAGS_BUTTON3`
- マウス移動: リアルタイム座標転送
- ホイール: 垂直・水平スクロール対応

## キーボード処理仕様

### 処理の流れ
1. `MainWindow::eventFilter()` が `windowHandle()` からの `KeyPress`/`KeyRelease` イベントを最初にキャッチし、特殊キー組み合わせを優先処理する
2. 特殊キーに該当しない場合は `MyView::onKeyEvent()` に委譲し、XKBスキャンコード（`event->nativeScanCode()`）をWindows仮想キーコードに変換（`GetVirtualKeyCodeFromKeycode(scancode, WINPR_KEYCODE_TYPE_XKB)`）した上でFreeRDPに送信する
3. 送信は `MyView::sendRdpKeyboardEvent()` が担い、`GetVirtualScanCodeFromVirtualKeyCode(vk, WINPR_KBD_TYPE_IBM_ENHANCED)` でスキャンコードに戻してから `freerdp_input_send_keyboard_event_ex()` を呼ぶ

### マジックキー（`Ctrl+Shift+Alt+<キー>`、KeyPressイベントのみ）

| キー組み合わせ | 動作 |
|---|---|
| `Ctrl+Shift+Alt+N` | 新規接続ダイアログを開く（`emitConnect`） |
| `Ctrl+Shift+Alt+F` | フルスクリーン切り替え |
| `Ctrl+Shift+Alt+D` | 表示スケール1倍/2倍切り替え（動的解像度有効時は解像度も追従） |
| `Ctrl+Shift+Alt+Backspace` | コマンドフォームの表示/非表示切り替え |
| `Ctrl+Shift+Alt+CapsLock` | リモートマシンのCapsLockをトグル（Shift+CapsLockの特殊シーケンス送信） |
| `Ctrl+Shift+Alt+F4` | フルスクリーン解除してウィンドウを閉じる |
| `Ctrl+Shift+Alt+<その他任意のキー>` | `Ctrl+Alt+<対象キー>` をリモートマシンに送信（例: リモートでのAlt+Tab） |

（`Ctrl+N` 単体ではなく `Ctrl+Shift+Alt+N` が新規接続のショートカットである点に注意。単体の `Alt` キー押下は常にイベントを消費し、意図しない動作を防止する。）

### チルダキー（`~`、日本語キーボードの半角/全角キー相当・XKBスキャンコード49）の特殊処理
- キープレス時: `m->tlde` フラグを立て、`VK_LMENU`（左Alt）のプレスをRDPに送信 → チルダキーが左Altとして機能する
- キーリリース時: `m->tlde` フラグを下ろし、`VK_LMENU` のリリースを送信
- チルダキーを押した状態で数字キー`1`を押すと、実際のチルダ文字（`VK_OEM_3`）を入力するための特殊シーケンス（左Altリリース → チルダキー押下 → チルダキー解放 → 左Alt再押下）をキーチャンクとして送信する

### キーチャンクシステム
複数のキーイベントを論理的な順序でグループ化して送信する仕組み。

- `addKeyChunk()`: 新しいチャンクの区切りを追加
- `addKey(DWORD vk, bool press)`: 仮想キーコード指定でキーイベントを追加
- `addNativeKey(quint32 native, bool pressed)`: ネイティブスキャンコード指定でキーイベントを追加
- `sendKeyboardModifiers(Qt::KeyboardModifiers mod)`: Ctrl/Shift/Altの左右修飾キー状態をまとめて送信
- `toggleCapsLock()`: 修飾キーをクリアしてから Shift押下→CapsLock押下→CapsLock解放→Shift解放 の順で送信

### 制限・注意点
- `eventFilter` は `windowHandle()` からのイベントのみを処理する（他のウィジェット由来のキーイベントは対象外）
- マジックキーは必ずKeyPressイベントでのみ判定される（KeyReleaseでは発火しない）
- `event->isAutoRepeat()` を適切に転送し、リモート側のオートリピート動作を保持する
- `ShortcutOverride` イベントの処理は `#if 0` で現在無効化されている
- デバッグ用の `qDebug()` ログ出力はコメントアウトされたまま残っている

## UI操作

- **File → Connect / Ctrl+Shift+Alt+N**: 接続ダイアログを開く
- **File → Disconnect**: 現在の接続を切断
- **View → Dynamic Resolution**: 動的解像度切り替えのトグル
- **ステータスバー**: 接続状態表示

## 現在の制限事項

- 音声転送、ファイル転送、クリップボード共有、プリンタリダイレクトは未サポート
- 接続履歴の保存・管理機能は未実装
- Linux環境のみサポート（X11/Waylandディスプレイサーバーが必要）
- FreeRDP 3.xに依存

## 既知の技術的懸念（今後整理したい点）

- `RdpSessionV1` 関連の分岐コードが `MainWindow.cpp` 各所に残っているが、実際には `RdpSessionV2` のみが使われており到達不能。V1関連コードを削除するか、ヘッダファイルへの分離も含めてリファクタリングを検討する余地がある
- `RdpSession`/`RdpSessionV1`/`RdpSessionV2`/`MyClientContext` は `MainWindow.h` ではなく `MainWindow.cpp` の翻訳単位内に定義されており、`MainWindow.h` 側には `friend class RdpSessionV2;` の前方宣言のみが残っている
