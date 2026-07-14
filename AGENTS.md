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

このシステムには複数バージョンのQtが入っており、素の `qmake` はQt5を指す場合がある。**Qt6用の `qmake6`（または `/usr/lib/qt6/bin/qmake`）を使うこと。**

```bash
qmake6 Radic.pro
make
```

- 出力実行ファイル: `Radic`
- Debug/Release のビルド成果物は `build/Qt_x_y_z-{Debug,Release}/` 配下に生成される
- `.ui` ファイルを変更してMakefileの依存関係だけでは`uic`再生成が反映されない場合は、上記の`qmake6`を再実行してMakefileを再生成すること。誤って古いQt5向けのqmakeでMakefileを再生成すると、ヘッダ探索パスやリンクするQtライブラリがQt5/Qt6混在になり、`QString::fromUtf8_helper`等の未定義参照リンクエラーになる。その場合は`qmake6`で正しく再生成した上で`make clean && make`でオブジェクトファイルを作り直すこと。

## 設定ファイル

- パス: `~/.config/soramimi.jp/Radic/Radic.ini`（`Global.h` の `ORGANIZATION_NAME`/`APPLICATION_NAME` と `MySettings` で管理）
- 保存項目: ウィンドウジオメトリ、最大化状態
- パスワードは保存されない（`MainWindow::on_action_connect_triggered` で明示的に空にしてから設定に書き込んでいる）
- 接続履歴の保存は未実装（`ConnectionDialog` は前回接続した host/user 等を `MySettings` から復元するのみ）

## ファイル構成

```
main.cpp                  - エントリーポイント（ApplicationGlobal初期化、QApplication起動）
Global.cpp/h              - アプリ全体の組織名/アプリ名/設定パスなどのグローバル定義
MainWindow.cpp/h/ui       - メインウィンドウ、RDP接続制御、FreeRDPコールバック、キーイベントフィルタ
MyView.cpp/h              - リモート画面描画・マウス/キーボード入力処理ウィジェット
ConnectionDialog.cpp/h/ui - 接続情報入力ダイアログ（ホスト名/ユーザー名の必須チェックあり）
VerifyCertificateDialog.cpp/h/ui - TLS証明書検証UI（新規証明書 / 変更された証明書の両方に対応）
rdpcert.h                 - 証明書情報の構造体・検証結果の定義（ヘッダオンリー。対応する.cppは削除済み）
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
- 画面更新タイマー（`m->update_timer`、10ms間隔）の管理。**ただしこのタイマーは動的解像度リサイズのカウントダウン（`resizeDynamic`）専用であり、実際の画面描画更新のトリガーではない**。画面更新はV1ならポーリングスレッド、V2なら`rdp_end_paint`コールバックが直接駆動する（後述）。
- フルスクリーン切り替え、動的解像度（Dynamic Resolution）切り替え
- `eventFilter` によるアプリケーション全体のキーイベント先取り（マジックキー処理、下記参照）
- ウィンドウ状態の永続化（`MySettings` 経由）

#### RdpSession 抽象化（V1/V2はどちらも現役、切り替えはソース編集が必要）
`MainWindow.cpp` 内に、FreeRDPのクライアントAPIを抽象化する `RdpSession` 基底クラスと `RdpSessionV1`/`RdpSessionV2` サブクラスが定義されている（ヘッダには分離されておらず `MainWindow.cpp` の翻訳単位内にのみ存在。`MainWindow.h` には `friend class RdpSessionV2;` の前方宣言のみ）。

- **RdpSessionV1**: レガシーAPI（`freerdp_new()` / `freerdp_context_new()`）を使う実装。画面取得は`start_rdp_thread`のポーリングループが`gdi->primary_buffer`をゼロコピーでラップして行う。
- **RdpSessionV2**: 新しいクライアントエントリポイントAPI（`freerdp_client_context_new()` + `RDP_CLIENT_ENTRY_POINTS`）を使う実装。`MyClientContext`（`rdpClientContext` のラッパー）を介してコンテキストを管理し、画面取得は`rdp_end_paint`コールバックが担う。

`MainWindow::MainWindow()`（`MainWindow.cpp:190`付近）の

```cpp
m->session = std::make_shared<RdpSessionV2>();
```

を`RdpSessionV1`に書き換えることでV1に切り替えられる（**実行時に切り替えるUIは無い**、ソースを編集して再ビルドする必要がある）。両方とも動作確認済みで、V1固有・V2固有の不具合をそれぞれ修正済み（下記「このセッションで修正した問題」参照）。コード中の`m->session->version() == RdpSession::V1`分岐は両方のパスで実際に使われるため、削除しないこと。

### MyView
リモートデスクトップ画面の表示と入力処理を担当するウィジェット。

- `QImage` によるフレームバッファ描画（`setImage`/`paintEvent`）。内部に描画専用のワーカースレッド（`startThread`）を持ち、`std::condition_variable`で新しいフレームの到着を待つ。**waitは述語付き**（`m->interrupted || !m->next_input_frame.isNull()`）にしてあり、`interrupted`フラグの読み書きは必ず`m->mutex`で保護すること。述語なしの単純な`cv.wait(lock)`に戻すと、`stopThread()`のタイミングによって通知を取り逃し、`thread.join()`が永久に返らなくなる（lost wakeup）。
- マウス入力（クリック、移動、ホイール）をFreeRDPに転送（`qtToRdpMouseButton` でボタンマッピング）
- ホイール回転量は`encodeWheelRotation()`でエンコードする。RDPの`PTR_FLAGS_WHEEL_NEGATIVE`は下位バイトを含めた9bit符号付き2の補数の一部（実質サインビット）なので、負方向は`abs(delta)`ではなく`0x100 - abs(delta)`を入れる必要がある（FreeRDP自身の`freerdp_client_send_wheel_event`のデコード規約に合わせている）。ここを単純な`abs(delta)`に戻すと、下スクロールなど負方向の回転量が不正な値になり、サーバー側での取り扱いによって「スクロールが途切れる」ような症状が再発する。
- キーボード入力をFreeRDPに転送（詳細は下記キーボード仕様を参照）
- 画面スケーリング（1倍/2倍）と座標変換（Qt座標系 ↔ RDP座標系）
- 複合キーシーケンスを順序通り送信するための「キーチャンク」キューイング機構（`addKeyChunk`/`addKey`/`addNativeKey`/`sendKeyChunk`）。`addKey`は`m->key_event_queue.back()`に追加するだけなので、**必ず直前に`addKeyChunk()`を呼んでから使うこと**（呼ばずに使うと空のdequeに対する`back()`で未定義動作になる）。

### ConnectionDialog
RDP接続情報（ホスト名/IP、ユーザー名、パスワード、ドメイン）の入力UI。デフォルト値としてホスト `192.168.0.20`、ドメイン `WORKGROUP` を持つ。`accept()`をオーバーライドしており、ホスト名またはユーザー名が空欄のままOKを押すと警告を出して接続処理に進まない。

### VerifyCertificateDialog / rdpcert
TLS証明書検証UI。`rdpcert::Certificate` 構造体（ヘッダオンリー、`rdpcert.cpp`は存在しない）と `CertResult`（Reject / AcceptPermanently / AcceptTemporarily）で結果を表現する。

- `setNewCertificate(cert)`: 初めて見るホストの証明書を検証する場合に使う
- `setChangedCertificate(oldCert, newCert)`: 以前信頼した証明書と異なる証明書が提示された場合に使う（`MainWindow::onRdpVerifyChangeCertificateEx`から呼ばれる）。以前の証明書のSubject/Issuer/Fingerprintを「Previously Trusted Certificate」グループボックスに並べて表示し、比較できるようにしている。
- `setWarningForFlags(flags)`: FreeRDPの`VERIFY_CERT_FLAG_MISMATCH`（ホスト名不一致、最も強いMITM兆候）/`VERIFY_CERT_FLAG_CHANGED`（証明書変更）を見て警告文言を切り替える。両方成立する場合はMISMATCHの文言が優先される。

### CommandForm
マジックキー（`Ctrl+Shift+Alt+Backspace`）で表示/非表示を切り替える補助コマンドパネル。`global->mainwindow`を使う箇所は必ずnullチェックすること（MainWindow.cpp側の同種コールバックとスタイルを合わせている）。

### MySettings
`QSettings` ベースの設定永続化。ウィンドウジオメトリ・最大化状態などをINI形式で保存。

### Global / ApplicationGlobal
組織名・アプリケーション名・設定ファイルパスなどアプリ全体の基本情報を保持するグローバルシングルトン。未使用だった空クラス`ApplicationSettings`は削除済み。

### joinpath
クロスプラットフォーム対応のパス結合ユーティリティ。文字列/QString双方に対応し、クォート除去・区切り文字正規化を行う。既知の癖として `joinpath("", "")` が `""` ではなく `"/"` を返す（現状の呼び出し元では問題化していない）。

## RDP接続・表示機能

- **対応プロトコル**: RDP
- **色深度**: 32bit（画面バッファは `PIXEL_FORMAT_RGB24` / `QImage::Format_RGB888`）
- **認証**: ユーザー名/パスワード認証、Windowsドメイン対応
- **解像度**: デフォルト1920x1080。**動的解像度切り替え機能が実装済みだがV2限定**（`disp_client_context()`がV1では常にnullを返すため、V1接続時は「Dynamic Resolution」メニュー項目自体を無効化してある。View メニューのトグル、および `Ctrl+Shift+Alt+D` のスケール切り替え時に連動して `resizeDynamicLater()`/`resizeDynamic()` が呼ばれる)
- **Graphics Pipeline (RDPGFX/H.264/AVC444)**: 有効化する設定はコード上あるが、`rdpgfx`チャンネルのプラグイン読み込みや`gdi_graphics_pipeline_init()`は実装されていないため実際には機能しない。**V1では明示的に無効化**している（`doConnect`で`FreeRDP_SupportGraphicsPipeline`を`RdpSession::V2`のときだけtrueにする）。V1で有効なままにすると、サーバーが存在しないGFXチャンネルのハンドシェイクを待ってタイムアウトするまで通常の描画にフォールバックせず、接続直後の初回描画が数秒遅延する不具合があった。V2側は同設定を維持しているが、GFXパイプライン自体が未実装なのは変わらないため、実際に使われているかどうかは要検証。
- **画面更新頻度**: タイマー駆動ではない。V1は`start_rdp_thread`のポーリングループ（前フレーム未消費ならスキップ、消費済みならその都度`gdi->primary_buffer`を取得）、V2は`rdp_end_paint`コールバック（FreeRDPが更新を処理し終えるたびに呼ばれる）がそれぞれ描画パイプラインを駆動する。
- **V2の描画スロットリング**: `rdp_end_paint`は元々呼ばれるたびに無条件でフレームバッファ全体（数MB）をディープコピーしていたため、スクロールのような更新頻度・エリアが大きい操作でRDP処理スレッドがコピー待ちになり、描画が遅延して見えた。`MainWindow::Private::v2_paint_pending`（`std::atomic<bool>`）で「前フレームがMyView側でまだ消費されていなければコピーをスキップする」スロットリングを追加し、V1のポーリングと同じ考え方でコピー回数を減らしている。このフラグは`MyView::ready`シグナル（1フレーム処理完了）で解除され、`doConnect()`の先頭でも切断・再接続をまたいで固着しないようリセットしている。**再接続時のリセットを忘れると、再接続後に永久に描画が始まらなくなるので注意。**
- **パフォーマンス最適化**: FastPath、ビットマップキャッシュ、RDP8圧縮、オフスクリーンサポート、グリフサポート、サーフェスコマンド、ネットワーク自動検出を有効化

### クリップボード共有

- FreeRDPの`cliprdr`静的チャネルを使い、`CF_UNICODETEXT`によるUnicodeプレーンテキストと`CF_DIB`によるビットマップ画像を双方向共有する。HTML、ファイルなどの形式は未対応。
- 画像送信はQt画像を32bit bottom-up `BI_RGB` DIB（BGRX）へ変換する。解像度はQt画像のdots-per-meterを使い、情報が無ければ96 DPI（3780 pixels/meter）を明示する。受信は24/32bit `BI_RGB`、top-down/bottom-upの両方に対応するが、アルファ透明度、`CF_DIBV5`、`BI_BITFIELDS`、圧縮DIBは未対応。破損データや過大確保を防ぐため、ヘッダ・寸法・stride・積算サイズを検証し、DIB全体を64 MiB以下に制限している。
- RDPクリップボード仕様は遅延レンダリングであり、形式一覧には画像寸法が含まれず、実データは貼り付け要求時に初めて転送される。Adobe Photoshopでは、ローカルから転送した画像の寸法が最初のペースト時には認識されず、2回目のペーストで反映される現象を確認済み。形式通知時の画像キャッシュ、`CF_DIB`の先頭配置、DIBへの明示的なDPI設定（既定96 DPI）を試しても改善しなかったため、RDP遅延レンダリングとPhotoshopの相互運用上の制約として追加対応を保留している。これらの既存対策を削除しても解決しない。
- ローカルの形式一覧を通知するときにテキストと画像をスナップショットし、リモートからの遅延データ要求にはその固定データを返す。要求時に`QClipboard`を再取得すると、X11/Waylandの遅延取得や画像編集アプリの複数回問い合わせで最初の画像サイズが不安定になるため、このキャッシュを外さないこと。画像編集アプリ向けに、画像とテキストが両方ある場合は`CF_DIB`を形式一覧の先頭に置く。
- `FreeRDP_RedirectClipboard`に加え、`FreeRDP_ClipboardFeatureMask`で`CLIPRDR_FLAG_LOCAL_TO_REMOTE | CLIPRDR_FLAG_REMOTE_TO_LOCAL`を明示的に有効化している。
- `cliprdrMonitorReady`でクライアント能力を通知し、ローカルの`QClipboard::dataChanged`時は実際に存在する`CF_UNICODETEXT`/`CF_DIB`を`ClientFormatList`で送信する。リモートからは`ServerFormatList`→`ClientFormatDataRequest`→`ServerFormatDataResponse`の順でデータを取得する。両形式が同時に提示された場合は画像を優先する。
- Windows側ではコピー直後の遅延レンダリング中にデータ要求すると`CB_RESPONSE_FAIL`が返る場合があるため、形式一覧への応答から30ms遅延して要求し、失敗時は間隔を広げながら最大5回再試行する。新しい形式一覧または切断時には世代番号を更新して古い再試行を無効化すること。この遅延・再試行を単純な即時要求に戻すと、リモート→ローカルの共有が最初の1回以降失敗する症状が再発する。
- リモート由来のQtクリップボードには`application/x-radic-remote-clipboard`内部MIMEマーカーを付け、`dataChanged`からリモートへ同じ内容を再通知するループを防いでいる。Qtクリップボードの更新時は既存内容を`clear()`してから設定する。
- Qtクリップボード操作はGUIスレッドで行う。RDPコールバックとの受け渡しには`QMetaObject::invokeMethod`を使い、要求形式と世代番号はスレッド間アクセスを考慮してatomicで管理する。

### マウス操作
- 左クリック: `PTR_FLAGS_BUTTON1`
- 右クリック: `PTR_FLAGS_BUTTON2`
- 中クリック: `PTR_FLAGS_BUTTON3`
- マウス移動: リアルタイム座標転送
- ホイール: 垂直・水平スクロール対応（回転量エンコードについては上記MyViewの節を参照）

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
- `addKey(DWORD vk, bool press)`: 仮想キーコード指定でキーイベントを追加（直前に`addKeyChunk()`が必要。上記MyViewの節を参照）
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
- **View → Dynamic Resolution**: 動的解像度切り替えのトグル（V1接続時は無効化される）
- **ステータスバー**: 接続状態表示
- 画面左上に常時FPS表示がある（`MyView::paintEvent`の`if (1)`ブロック）。開発用の名残で、必要なくなったらメンテナが自分で外す予定。今のところ現状維持。

## 現在の制限事項

- クリップボード共有はUnicodeプレーンテキストとビットマップ画像に対応（ファイル、HTML、アルファ透明度などは未対応）
- Adobe Photoshopでは、リダイレクトした画像の寸法が最初のペーストでは認識されず、2回目に反映される場合がある（調査済み・追加対応保留）
- 音声転送、ファイル転送、プリンタリダイレクトは未サポート
- 接続履歴の保存・管理機能は未実装
- Linux環境のみサポート（X11/Waylandディスプレイサーバーが必要）
- FreeRDP 3.xに依存
- Graphics Pipeline (RDPGFX)は設定上は触れるが実装されておらず機能しない

## このセッションで修正した問題（要約）

過去の対話で見つかり、修正済みの問題。再発防止のため経緯を残す。

1. **V1接続時の初回描画が数秒遅延する問題** — `SupportGraphicsPipeline`をV1で無効化（未実装のGFXチャンネルのハンドシェイクタイムアウト待ちを回避）
2. **マウスホイールのスクロールが途切れる問題** — `PTR_FLAGS_WHEEL_NEGATIVE`時の回転量エンコードを2の補数に修正
3. **V2の描画レスポンスがV1より遅い問題** — `rdp_end_paint`に`v2_paint_pending`スロットリングを追加し、無駄な全画面コピーを削減
4. **証明書変更時にMITM兆候がUIに出ない問題** — `VerifyCertificateDialog::setChangedCertificate`/`setWarningForFlags`を追加
5. **MyViewのcv.wait()にlost wakeupのリスク** — 述語付きwaitに変更
6. **V1で`channelConnected`/`channelDisconnected`がV2用の`MyClientContext*`にキャストする型混同のリスク** — V2セッション時のみ実行するよう変更
7. **ConnectionDialogでホスト名/ユーザー名が空でも接続できる問題** — `accept()`をオーバーライドして検証を追加
8. **V1でDynamic Resolutionメニューが何も起きずに有効に見える問題** — V1接続時はメニューを無効化
9. **クリーンアップ** — 空だった`rdpcert.cpp`を削除、`Global.h`の未使用クラス`ApplicationSettings`を削除、`CommandForm.cpp`のnullチェック追加
10. **テキストと画像のクリップボード共有を追加** — `cliprdr`による双方向`CF_UNICODETEXT`/`CF_DIB`共有を実装。能力交渉・方向別FeatureMask・Qt側の再通知ループ防止に加え、Windows側のコピー直後に`CB_RESPONSE_FAIL`が返る問題を遅延要求と世代管理付きリトライで回避

## 既知の技術的懸念（今後整理したい点）

- `RdpSessionV1`/`RdpSessionV2`はどちらも現役で使われているが、切り替えは`MainWindow.cpp`のソース編集（`make_shared<RdpSessionV1/V2>`）でしか行えない。設定やコマンドライン引数でのランタイム切り替えは無い
- `RdpSession`/`RdpSessionV1`/`RdpSessionV2`/`MyClientContext` は `MainWindow.h` ではなく `MainWindow.cpp` の翻訳単位内に定義されており、`MainWindow.h` 側には `friend class RdpSessionV2;` の前方宣言のみが残っている
- Graphics Pipeline関連の設定（`FreeRDP_GfxH264`/`FreeRDP_GfxAVC444`等）はV2で有効なままだが、対応する実装（`gdi_graphics_pipeline_init`やrdpgfxチャンネル）が無いため、実際に効果があるのか未検証
- FPS表示（常時ON）、`joinpath("", "")`の挙動、`MySettings.h`の未使用前方宣言は既知だが、メンテナの意向で現状維持（他のエージェントが「修正」として触らないよう明記）
