# Radic キーボードアサイン仕様書

## 概要

Radicアプリケーションにおけるキーボード入力の処理と特殊キーアサインについての詳細仕様です。

## キーボード処理の流れ

### 1. イベントフィルタリング
- `MainWindow::eventFilter()`でウィンドウハンドルのキーイベントを最初にキャッチ
- `QEvent::KeyPress`と`QEvent::KeyRelease`イベントを処理
- 特殊キー組み合わせの判定とアプリケーション制御機能を優先処理

### 2. 通常キー処理
- 特殊キーに該当しない場合は`MyView::onKeyEvent()`に委譲
- XKBスキャンコードからWindows仮想キーコードに変換
- FreeRDPを通じてリモートマシンに送信

## 特殊キーアサイン（マジックキー）

### マジックキー組み合わせの定義
**基本パターン**: `Ctrl + Shift + Alt` + 対象キー（KeyPressイベントのみ）

```cpp
bool isSpecialModifiersPressed = (pressed && alt && ctrl && shift);
```

### 対応する特殊キーアサイン

#### 1. フルスクリーン切り替え
- **キー組み合わせ**: `Ctrl + Shift + Alt + F`
- **動作**: フルスクリーンモードのオン/オフ切り替え
- **実装**: `setFullScreen(!isFullScreen())`

#### 2. 表示スケール切り替え
- **キー組み合わせ**: `Ctrl + Shift + Alt + D`
- **動作**: 表示スケールを1倍と2倍で切り替え
- **実装**: `ui->widget_view->setScale(scale == 1 ? 2 : 1)`
- **副作用**: 動的解像度が有効な場合は解像度調整を実行

#### 3. コマンドフォーム表示切り替え
- **キー組み合わせ**: `Ctrl + Shift + Alt + Backspace`
- **動作**: コマンドフォームの表示/非表示切り替え
- **実装**: `showCommandForm(!ui->widget_view->isCommandFormVisible())`

#### 4. CapsLock切り替え
- **キー組み合わせ**: `Ctrl + Shift + Alt + CapsLock`
- **動作**: リモートマシンのCapsLockを切り替え
- **実装**: 特殊なキーシーケンスを送信（Shift + CapsLock）

#### 5. 汎用キー送信
- **キー組み合わせ**: `Ctrl + Shift + Alt + 任意のキー`
- **動作**: `Ctrl + Alt + 対象キー`をリモートマシンに送信
- **用途**: リモートマシンでのAlt+Tabなどの操作

## 特殊キー処理の詳細

### 1. チルダキー（~）の特殊処理

#### X11スキャンコード定義
```cpp
enum XNativeScanCode {
    XK_TLDE = 49,  // チルダキー（~）
    XK_1 = 10,     // 数字キー1
    // ...
};
```

#### チルダキーの動作
- **キープレス時**: 
  - `m->tlde`フラグをtrueに設定
  - `VK_LMENU`（左Alt）キーのプレス状態を送信
- **キーリリース時**:
  - `m->tlde`フラグをfalseに設定
  - `VK_LMENU`（左Alt）キーのリリース状態を送信
- **効果**: チルダキーが左Altキーとして機能

#### チルダ + 1キーの組み合わせ
- **条件**: チルダキーが押された状態で数字キー1を押下
- **動作シーケンス**:
  1. 左Alt（VK_LMENU）をリリース
  2. キーチャンクを追加
  3. `VK_OEM_3`（チルダキー）をプレス
  4. キーチャンクを追加
  5. `VK_OEM_3`（チルダキー）をリリース
  6. キーチャンクを追加
  7. 左Alt（VK_LMENU）をプレス

### 2. Altキーの処理
- **単独のAltキー**: 常にイベントを消費（`return true`）
- **目的**: 意図しないAltキーの動作を防止

## キーボード修飾キーの処理

### 修飾キー状態の管理
```cpp
Qt::KeyboardModifiers mod = e->modifiers() & Qt::KeyboardModifierMask;
bool ctrl = mod & Qt::ControlModifier;
bool alt = mod & Qt::AltModifier;
bool shift = mod & Qt::ShiftModifier;
```

### 修飾キー変更の検出
- `m->last_keyboard_modifier`で前回の修飾キー状態を保持
- 修飾キー状態が変更された場合にのみ更新

## 通常キー処理の流れ

### MyView::onKeyEvent()の処理
1. **KeyPressイベント判定**: `event->type() == QEvent::KeyPress`
2. **スキャンコード変換**: XKBスキャンコード → Windows仮想キーコード
   ```cpp
   auto vk = GetVirtualKeyCodeFromKeycode(event->nativeScanCode(), WINPR_KEYCODE_TYPE_XKB);
   ```
3. **RDPキーイベント送信**: 
   ```cpp
   sendRdpKeyboardEvent({vk, pressed, event->isAutoRepeat()});
   ```

### RDPキーイベント送信の実装
```cpp
bool MyView::sendRdpKeyboardEvent(Key const &k)
{
    if (m->rdp_instance && m->rdp_instance->context && m->rdp_instance->context->input) {
        auto code = GetVirtualScanCodeFromVirtualKeyCode(k.vk, WINPR_KBD_TYPE_IBM_ENHANCED);
        freerdp_input_send_keyboard_event_ex(m->rdp_instance->context->input, k.pressed, k.autorepeat, code);
        return true;
    }
    return false;
}
```

## キーイベントキューシステム

### キーチャンクの概念
- 複数のキーイベントを論理的にグループ化
- `addKeyChunk()`で新しいチャンクを作成
- `addKey(DWORD vk, bool press)`でキーイベントを追加
- `addNativeKey(quint32 native, bool pressed)`でネイティブキーを追加

### 修飾キー送信の実装
```cpp
void MyView::sendKeyboardModifiers(Qt::KeyboardModifiers mod)
{
    addKeyChunk();
    addKey(VK_LCONTROL, mod & Qt::ControlModifier);
    addKey(VK_RCONTROL, false);
    addKey(VK_CONTROL, false);
    addKey(VK_LSHIFT, mod & Qt::ShiftModifier);
    addKey(VK_RSHIFT, false);
    addKey(VK_SHIFT, false);
    addKey(VK_LMENU, mod & Qt::AltModifier);
    addKey(VK_RMENU, false);
    addKey(VK_MENU, false);
}
```

### CapsLock切り替えの実装
```cpp
void MyView::toggleCapsLock()
{
    sendKeyboardModifiers(Qt::NoModifier);  // 修飾キーをクリア
    
    addKeyChunk();
    addKey(VK_LSHIFT, true);                // Shiftキープレス
    addKeyChunk();
    addKey(VK_CAPITAL, true);               // CapsLockプレス
    
    addKeyChunk();
    addKey(VK_CAPITAL, false);              // CapsLockリリース
    addKeyChunk();
    addKey(VK_LSHIFT, false);               // Shiftキーリリース
}
```

## キー変換とマッピング

### XKB → Windows仮想キーコード変換
- **変換関数**: `GetVirtualKeyCodeFromKeycode(scancode, WINPR_KEYCODE_TYPE_XKB)`
- **逆変換**: `GetVirtualScanCodeFromVirtualKeyCode(vk, WINPR_KBD_TYPE_IBM_ENHANCED)`
- **キーボードタイプ**: IBM Enhanced Keyboard（104キー）

### 主要なWindows仮想キーコード
- `VK_LCONTROL`: 左Controlキー
- `VK_LSHIFT`: 左Shiftキー
- `VK_LMENU`: 左Altキー（Menuキー）
- `VK_CAPITAL`: CapsLockキー
- `VK_OEM_3`: チルダキー（`）

## イベント処理の優先順位

1. **最高優先**: 特殊キー組み合わせ（マジックキー）
2. **高優先**: チルダキーとその組み合わせ
3. **中優先**: 単独Altキー（無効化）
4. **低優先**: その他のキー（MyView::onKeyEventに委譲）

## デバッグとログ

### デバッグ出力（コメントアウト状態）
```cpp
// qDebug() << Q_FUNC_INFO << pressed << QString::asprintf("%08x", key) << mod << e->nativeScanCode();
```

- キーの16進表示
- 修飾キー状態
- ネイティブスキャンコード

## 制限事項と注意点

### 1. イベントフィルタの対象
- `windowHandle()`からのイベントのみ処理
- 他のウィジェットからのキーイベントは対象外

### 2. 特殊キーの制約
- 必ずKeyPressイベントでのみ処理
- KeyReleaseイベントでは特殊機能は動作しない

### 3. オートリピートの処理
- `event->isAutoRepeat()`フラグを適切に転送
- リモートマシンでのオートリピート動作を保持

### 4. ShortcutOverrideイベント
- 現在は無効化されている（`#if 0`でコメントアウト）
- 必要に応じて有効化可能

## 使用例

### リモートマシンでAlt+Tabを実行
1. `Ctrl + Shift + Alt + Tab`を押下
2. 汎用キー送信機能により`Ctrl + Alt + Tab`がリモートに送信

### フルスクリーンモードでの操作
1. `Ctrl + Shift + Alt + F`でフルスクリーン切り替え
2. `Ctrl + Shift + Alt + Backspace`でコマンドフォーム表示

### 日本語キーボードでの特殊操作
1. チルダキー（半角/全角キー）で左Altとして動作
2. チルダ + 1キーで実際のチルダ文字を入力

この仕様により、LinuxクライアントからWindowsリモートマシンへの効率的なキーボード操作が実現されています。
