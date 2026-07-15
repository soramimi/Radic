#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "ConnectionDialog.h"
#include "MySettings.h"
#include <QPainter>
#include <QWindow>
#include <QApplication>
#include <QClipboard>
#include <QMetaObject>
#include <QMimeData>
#include <QThread>
#include <QtEndian>
#include <atomic>
#include <limits>
#include <thread>
#include "Global.h"
#include "VerifyCertificateDialog.h"
#include "rdpcert.h"

#define RDP_SESSION RdpSessionV2

struct MyClientContext {
	rdpClientContext rdpcc;
	MainWindow *self = nullptr;
	DispClientContext *disp = nullptr;
};

class RdpSession {
public:
	virtual RdpSessionVersion version() = 0;
	virtual void context_new(MainWindow *self) = 0;
	virtual void context_free() = 0;
	virtual freerdp *rdp_instance() = 0;
	virtual rdpContext *rdp_context() = 0;
	virtual s_disp_client_context *disp_client_context() = 0;
	
	rdpSettings *rdp_settings()
	{
		auto *inst = rdp_instance();
		return (inst && inst->context) ? inst->context->settings : nullptr;
	}
	
	rdpGdi *rdp_gdi()
	{
		auto *inst = rdp_instance();
		return (inst && inst->context) ? inst->context->gdi : nullptr;
	}
};

class RdpSessionV1 : public RdpSession {
public:
	
	union {
		freerdp *rdp = nullptr;
	} d = {};
	
	RdpSessionVersion version() { return V1; }
	
	void context_new(MainWindow *self)
	{
		d.rdp = freerdp_new();
		freerdp_context_new(d.rdp);
	}

	void context_free()
	{
		freerdp_free(d.rdp);
		d.rdp = nullptr;
	}

	freerdp *rdp_instance()
	{
		return d.rdp;
	}
	
	rdpContext *rdp_context()
	{
		return d.rdp->context;
	}
		
	s_disp_client_context *disp_client_context()
	{
		return nullptr;
	}
};

class RdpSessionV2 : public RdpSession {
public:
	RdpSessionVersion version() { return V2; }

	union {
		rdpContext *rdp;
		MyClientContext *cc;
	} d = {};

	void context_new(MainWindow *self)
	{
		RDP_CLIENT_ENTRY_POINTS entry = {};
		entry.Version = RDP_CLIENT_INTERFACE_VERSION;
		entry.Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
		entry.ContextSize = sizeof(MyClientContext);
		entry.GlobalInit = MainWindow::clientGlobalInit;
		entry.GlobalUninit = MainWindow::clientGlobalUninit;
		entry.ClientNew = MainWindow::clientContextNew;
		entry.ClientFree = MainWindow::clientContextFree;
		entry.ClientStart = MainWindow::clientContextStart;
		entry.ClientStop = MainWindow::clientContextStop;

		d.rdp = freerdp_client_context_new(&entry);
		d.cc->self = self;

		self->setupRdpContext(d.rdp);
	}

	void context_free()
	{
		freerdp_client_context_free(d.rdp);
		d.rdp = nullptr;
	}

	freerdp *rdp_instance()
	{
		return d.rdp ? d.rdp->instance : nullptr;
	}
	
	rdpContext *rdp_context()
	{
		return d.rdp;
	}
	
	s_disp_client_context *disp_client_context()
	{
		return d.cc->disp;
	}
};

struct MainWindow::Private {
	std::shared_ptr<RdpSession> session;
	QTimer update_timer;
	bool connected = false;
	QSize size { 1920, 1080 };
	std::thread rdp_thread;
	bool interrupted = false;
	int dynamic_resize_counter = 0;

	Qt::KeyboardModifiers last_keyboard_modifier = (Qt::KeyboardModifier)-1;

#if 1
	constexpr static UINT32 rdp_pixel_format = PIXEL_FORMAT_RGB24;
	constexpr static QImage::Format screen_image_foramt = QImage::Format_RGB888;
#else
	constexpr static UINT32 rdp_pixel_format = PIXEL_FORMAT_RGBX32;
	constexpr static QImage::Format screen_image_foramt = QImage::Format_RGBX8888;
#endif

	QImage screen_image;

	bool tlde = false;

	// V2: 直前にMyViewへ渡したフレームがまだ消費されていない間は
	// rdp_end_paintでの全画面コピーをスキップする(V1のisNullによる
	// スロットリングと同じ狙い)。screen_imageはGDIが継続的に書き込む
	// ライブバッファなので、スキップしても最新の累積状態は失われない。
	std::atomic<bool> v2_paint_pending { false };

	CliprdrClientContext *cliprdr = nullptr;
	bool updating_remote_clipboard = false;
	std::atomic<UINT32> requested_clipboard_format { 0 };
	std::atomic<quint64> remote_clipboard_generation { 0 };
	int remote_clipboard_request_attempts = 0;
	QString local_clipboard_text;
	QImage local_clipboard_image;
	bool local_clipboard_has_text = false;
	bool local_clipboard_has_image = false;
};

static constexpr char REMOTE_CLIPBOARD_MIME[] = "application/x-radic-remote-clipboard";
static constexpr qsizetype MAX_CLIPBOARD_IMAGE_BYTES = 64 * 1024 * 1024;
static constexpr LONG DEFAULT_IMAGE_PIXELS_PER_METER = 3780; // 96 DPI

static QByteArray imageToDib(const QImage &source)
{
	if (source.isNull()) return {};
	QImage image = source.convertToFormat(QImage::Format_RGB32);
	const qint64 stride = static_cast<qint64>(image.width()) * 4;
	const qint64 pixelBytes = stride * image.height();
	if (pixelBytes <= 0 || pixelBytes > MAX_CLIPBOARD_IMAGE_BYTES - static_cast<qsizetype>(sizeof(BITMAPINFOHEADER)) ||
		pixelBytes > std::numeric_limits<UINT32>::max()) return {};

	QByteArray dib(sizeof(BITMAPINFOHEADER) + pixelBytes, Qt::Uninitialized);
	BITMAPINFOHEADER header = {};
	header.biSize = sizeof(BITMAPINFOHEADER);
	header.biWidth = image.width();
	header.biHeight = image.height(); // positive: bottom-up DIB
	header.biPlanes = 1;
	header.biBitCount = 32;
	header.biCompression = BI_RGB;
	header.biSizeImage = static_cast<DWORD>(pixelBytes);
	header.biXPelsPerMeter = image.dotsPerMeterX() > 0 ? image.dotsPerMeterX() : DEFAULT_IMAGE_PIXELS_PER_METER;
	header.biYPelsPerMeter = image.dotsPerMeterY() > 0 ? image.dotsPerMeterY() : DEFAULT_IMAGE_PIXELS_PER_METER;
	memcpy(dib.data(), &header, sizeof(header));

	auto *dst = reinterpret_cast<uchar *>(dib.data() + sizeof(header));
	for (int y = 0; y < image.height(); ++y) {
		const QRgb *src = reinterpret_cast<const QRgb *>(image.constScanLine(image.height() - 1 - y));
		for (int x = 0; x < image.width(); ++x) {
			dst[x * 4 + 0] = qBlue(src[x]);
			dst[x * 4 + 1] = qGreen(src[x]);
			dst[x * 4 + 2] = qRed(src[x]);
			dst[x * 4 + 3] = 0;
		}
		dst += stride;
	}
	return dib;
}

static QImage dibToImage(const QByteArray &dib)
{
	if (dib.size() < static_cast<qsizetype>(sizeof(BITMAPINFOHEADER)) ||
		dib.size() > MAX_CLIPBOARD_IMAGE_BYTES) return {};

	BITMAPINFOHEADER header = {};
	memcpy(&header, dib.constData(), sizeof(header));
	if (header.biSize < sizeof(BITMAPINFOHEADER) || header.biSize > static_cast<DWORD>(dib.size()) ||
		header.biWidth <= 0 || header.biHeight == 0 || header.biHeight == std::numeric_limits<LONG>::min() ||
		header.biPlanes != 1 || (header.biBitCount != 24 && header.biBitCount != 32) ||
		header.biCompression != BI_RGB) return {};

	const qint64 width = header.biWidth;
	const qint64 height = std::abs(static_cast<qint64>(header.biHeight));
	const qint64 stride = ((width * header.biBitCount + 31) / 32) * 4;
	const qint64 pixelBytes = stride * height;
	if (width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max() ||
		pixelBytes <= 0 || pixelBytes > MAX_CLIPBOARD_IMAGE_BYTES ||
		static_cast<qint64>(header.biSize) + pixelBytes > dib.size()) return {};

	QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGB32);
	if (image.isNull()) return {};
	const auto *pixels = reinterpret_cast<const uchar *>(dib.constData() + header.biSize);
	for (int y = 0; y < image.height(); ++y) {
		const int srcY = header.biHeight > 0 ? image.height() - 1 - y : y;
		const uchar *src = pixels + static_cast<qint64>(srcY) * stride;
		QRgb *dst = reinterpret_cast<QRgb *>(image.scanLine(y));
		for (int x = 0; x < image.width(); ++x) {
			const int offset = x * (header.biBitCount / 8);
			dst[x] = qRgb(src[offset + 2], src[offset + 1], src[offset]);
		}
	}
	return image;
}

void MainWindow::setupRdpContext(rdpContext *rdpcx)
{
	rdpcx->update->EndPaint = MainWindow::rdp_end_paint;
	rdpcx->update->DesktopResize = MainWindow::rdp_resize_display;
	// rdpcx->update->PlaySound = MainWindow::rdp_play_sound;
}

MainWindow::MainWindow(QWidget *parent)
	: QMainWindow(parent)
	, ui(new Ui::MainWindow)
	, m(new Private)
{
	ui->setupUi(this);

	m->session = std::make_shared<RDP_SESSION>();

	if (rdp_session_version() == RdpSessionVersion::V1) {
		// V1(freerdp_new/freerdp_context_new)はDisplay Control拡張を持たないため、
		// Dynamic Resolutionを有効にしても何も起きない。無効化して分かるようにする。
		ui->action_view_dynamic_resolution->setEnabled(false);
		ui->action_view_dynamic_resolution->setToolTip(tr("Not supported in this session mode"));
	}

	setDefaultWindowTitle();

	qApp->installEventFilter(this);

	connect(this, &MainWindow::emitConnect, this, &MainWindow::on_action_connect_triggered);
	connect(this, &MainWindow::emitDisconnect, this, &MainWindow::on_action_disconnect_triggered);

	connect(&m->update_timer, &QTimer::timeout, this, &MainWindow::onIntervalTimer);
	m->update_timer.setInterval(10);
	m->update_timer.start();

	connect(this, &MainWindow::requestUpdateScreen, this, &MainWindow::updateScreen);

	// MyView側が1フレームの処理を終えたら、V2のペイント待機フラグを解除する
	connect(ui->widget_view, &MyView::ready, this, [this]() {
		m->v2_paint_pending = false;
	});
	connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this]() {
		if (m->updating_remote_clipboard) return;
		const QMimeData *mime = QApplication::clipboard()->mimeData();
		if (mime && mime->hasFormat(REMOTE_CLIPBOARD_MIME)) return;
		sendClipboardFormatList();
	});

	{
		Qt::WindowStates state = windowState();
		MySettings settings;

		settings.beginGroup("MainWindow");
		bool maximized = settings.value("Maximized").toBool();
		restoreGeometry(settings.value("Geometry").toByteArray());
		settings.endGroup();
		if (maximized) {
			state |= Qt::WindowMaximized;
			setWindowState(state);
		}
	}

	// フォーカスポリシーの設定
	ui->widget_view->setFocusPolicy(Qt::StrongFocus);
	setFocusPolicy(Qt::StrongFocus);
}

MainWindow::~MainWindow()
{
	doDisconnect();
	delete m;
	delete ui;
}

RdpSessionVersion MainWindow::rdp_session_version()
{
	return m->session->version();
}

void MainWindow::setDefaultWindowTitle()
{
	QString title = "Radic - Remote Desktop Client";
	setWindowTitle(title);
}

freerdp *MainWindow::rdp_instance()
{
	return m->session->rdp_instance();
}

rdpSettings *MainWindow::rdp_settings()
{
	return m->session->rdp_settings();
}

s_disp_client_context *MainWindow::disp_client_context()
{
	return m->session->disp_client_context();
}

rdpGdi *MainWindow::rdp_gdi()
{
	return m->session->rdp_gdi();
}

QSize MainWindow::newSize() const
{
	int scale = ui->widget_view->scale();
	int w = ui->widget_view->width() / scale;
	int h = ui->widget_view->height() / scale;
	w = std::clamp(w, DISPLAY_CONTROL_MIN_MONITOR_WIDTH, DISPLAY_CONTROL_MAX_MONITOR_WIDTH);
	h = std::clamp(h, DISPLAY_CONTROL_MIN_MONITOR_HEIGHT, DISPLAY_CONTROL_MAX_MONITOR_HEIGHT);
	return {w, h};
}

BOOL MainWindow::clientGlobalInit()
{
	qDebug() << Q_FUNC_INFO;
	return true;
}

void MainWindow::clientGlobalUninit()
{
	qDebug() << Q_FUNC_INFO;
}

void MainWindow::initInstance(freerdp *instance)
{
	instance->PreConnect = rdp_pre_connect;
	instance->PostConnect = rdp_post_connect;
	instance->PostDisconnect = rdp_post_disconnect;
	instance->Authenticate = rdp_authenticate;
	instance->VerifyCertificateEx = rdp_verify_certificate_ex;
	instance->VerifyChangedCertificateEx = rdp_verify_change_certificate_ex;
}

BOOL MainWindow::clientContextNew(freerdp *instance, rdpContext *context)
{
	qDebug() << Q_FUNC_INFO;
	if (!instance || !context)
		return false;

	MyClientContext *ctx = reinterpret_cast<MyClientContext *>(context);
	ctx->self->initInstance(instance);
	return true;
}

void MainWindow::clientContextFree(freerdp *instance, rdpContext *context)
{
	qDebug() << Q_FUNC_INFO;
}

int MainWindow::clientContextStart(rdpContext *context)
{
	qDebug() << Q_FUNC_INFO;
	return 0;
}

int MainWindow::clientContextStop(rdpContext *context)
{
	qDebug() << Q_FUNC_INFO;
	return 0;
}

void MainWindow::doConnect(const QString &hostname, const QString &username, const QString &password, const QString &domain)
{
	if (m->connected) {
		doDisconnect();
	}

	m->screen_image = {};
	m->v2_paint_pending = false;
	m->cliprdr = nullptr;
	m->requested_clipboard_format = 0;
	m->remote_clipboard_generation++;
	m->remote_clipboard_request_attempts = 0;

	// 動的解像度が有効な場合は、現在のビューサイズに合わせる
	if (isDynamicResizingEnabled()) {
		m->size = newSize();
	}

	m->interrupted = false;

	m->session->context_new(this);

	if (rdp_session_version() == RdpSessionVersion::V1) {
		initInstance(rdp_instance());
	}

	// 接続設定
	rdpSettings *settings = rdp_instance()->context->settings;
	freerdp_settings_set_string(settings, FreeRDP_ServerHostname, hostname.toUtf8().constData());
	freerdp_settings_set_string(settings, FreeRDP_Username, username.toUtf8().constData());
	freerdp_settings_set_string(settings, FreeRDP_Password, password.toUtf8().constData());
	freerdp_settings_set_string(settings, FreeRDP_Domain, domain.toUtf8().constData());
	freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, m->size.width());
	freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, m->size.height());

	if (rdp_session_version() == RdpSessionVersion::V2) {
		// Display拡張を有効化（動的解像度変更のため）
		freerdp_settings_set_bool(settings, FreeRDP_SupportDisplayControl, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DynamicResolutionUpdate, TRUE);
	}

	// 安全なパフォーマンス最適化設定のみ適用
	freerdp_settings_set_bool(settings, FreeRDP_FastPathOutput, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_FastPathInput, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_SupportDynamicChannels, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE);
	freerdp_settings_set_uint32(settings, FreeRDP_ClipboardFeatureMask, CLIPRDR_FLAG_LOCAL_TO_REMOTE | CLIPRDR_FLAG_REMOTE_TO_LOCAL);

	// V1はGraphics Pipeline(rdpgfx)チャンネルを実装していないため、有効化するとサーバー側の
	// チャンネルハンドシェイクがタイムアウトするまで通常の描画オーダーへフォールバックされず、
	// 初回描画が遅延する。V1では明示的に無効化する。
	freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, rdp_session_version() == RdpSessionVersion::V2);
	freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, true);
	freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, true);
	freerdp_settings_set_bool(settings, FreeRDP_GfxH264, true);
	freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, false);
	freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);

#if 0 // RdpSessionVersion::V1 では、クリップボードの正常な動作が確認できなかったため一旦無効化しておく
	// freerdp_client_context_new()を使うV2はクライアントエントリポイントが
	// チャネルを読み込む。レガシーなV1では明示的にadd-inを読み込む必要がある。
	if (rdp_session_version() == RdpSessionVersion::V1 && !freerdp_client_load_addins(rdp_instance()->context->channels, settings)) {
		
		QMessageBox::critical(this, "Error", "Failed to load FreeRDP channels");
		m->session->context_free();
		return;
	}
#endif

	// 接続実行
	if (freerdp_connect(rdp_instance())) {
		m->connected = true;
		m->update_timer.start();
		ui->widget_view->setRdpInstance(rdp_instance());

		start_rdp_thread();

		statusBar()->showMessage("Connected to " + hostname);

		QString title = hostname + " - Radic";
		setWindowTitle(title);
	} else {
		QMessageBox::critical(this, "Error", "Failed to connect to " + hostname);
		m->session->context_free();
	}
}

void MainWindow::doDisconnect()
{
	ui->widget_view->setRdpInstance(nullptr);
	m->update_timer.stop();

	m->interrupted = true;
	if (m->rdp_thread.joinable()) {
		m->rdp_thread.join();
	}

	if (rdp_instance()) {
		freerdp_disconnect(rdp_instance());
		m->session->context_free();
	}
	m->connected = false;
	statusBar()->showMessage("Disconnected");

	QImage image(m->size.width(), m->size.height(), m->screen_image_foramt);
	image.fill(Qt::black);
	m->screen_image = image;
	if (rdp_session_version() == RdpSessionVersion::V2) {
		image = image.copy();
	}
	ui->widget_view->setImage(image, QRect{});

	setDefaultWindowTitle();
}

void MainWindow::onIntervalTimer()
{
	if (m->interrupted) return;
	if (!m->connected) return;

	if (m->dynamic_resize_counter > 0) {
		m->dynamic_resize_counter--;
		if (m->dynamic_resize_counter == 0) {
			resizeDynamic();
			return;
		}
	}
}

void MainWindow::updateScreen()
{
	if (m->interrupted) return;
	if (!m->connected) return;

	QImage image;
	std::swap(image, m->screen_image);
	if (!image.isNull()) {
		if (rdp_session_version() == RdpSessionVersion::V1) {
			ui->widget_view->setImage(image, QRect{});
		}
	}
}

void MainWindow::updateScreen2(QImage const &image, QRect const &rect)
{
	if (m->interrupted) return;
	if (!m->connected) return;

	if (!image.isNull()) {
		if (rdp_session_version() == RdpSessionVersion::V2) {
			ui->widget_view->setImage(image, rect);
		}
	}
}

void MainWindow::showCommandForm(bool show)
{
	if (isFullScreen()) {
		ui->widget_view->showCommandForm(show);
	}
}

void MainWindow::setFullScreen(bool full_screen)
{
	ui->widget_view->sendKeyboardModifiers(Qt::NoModifier);
	if (full_screen) {
		menuBar()->setVisible(false);
		statusBar()->setVisible(false);
		showFullScreen();
	} else {
		showCommandForm(false);
		menuBar()->setVisible(true);
		statusBar()->setVisible(true);
		showNormal();
	}
}

// ref. /usr/share/X11/xkb/keycodes/evdev
enum XNativeScanCode {
	XK_1 = 10,
	XK_2,
	XK_3,
	XK_4,
	XK_5,
	XK_6,
	XK_7,
	XK_8,
	XK_9,
	XK_0,
	XK_TLDE = 49,
};

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == windowHandle()) {
		if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
			bool pressed = (event->type() == QEvent::KeyPress);
			QKeyEvent *e = static_cast<QKeyEvent *>(event);
			int key = e->key();
			Qt::KeyboardModifiers mod = e->modifiers() & Qt::KeyboardModifierMask;
			bool ctrl = mod & Qt::ControlModifier;
			bool alt = mod & Qt::AltModifier;
			bool shift = mod & Qt::ShiftModifier;
			bool isSpecialModifiersPressed = (pressed && alt && ctrl && shift);
			if (mod != m->last_keyboard_modifier) {
				m->last_keyboard_modifier = mod;
			}
			if (pressed &&  key == Qt::Key_Backspace) {
				if (isSpecialModifiersPressed) {
					showCommandForm(!ui->widget_view->isCommandFormVisible());
					return true;
				}
			} else if (pressed && key == Qt::Key_N) {
				if (isSpecialModifiersPressed) {
					emit emitConnect();
					return true;
				}
			} else if (pressed && key == Qt::Key_F) {
				if (isSpecialModifiersPressed) {
					setFullScreen(!isFullScreen());
					return true;
				}
			} else if (pressed && key == Qt::Key_D) {
				if (isSpecialModifiersPressed) {
					ui->widget_view->sendKeyboardModifiers(Qt::NoModifier);
					if (ui->widget_view->scale() == 1) {
						ui->widget_view->setScale(2);
					} else {
						ui->widget_view->setScale(1);
					}
					if (isDynamicResizingEnabled()) {
						resizeDynamicLater();
					}
					return true;
				}
			} else if (pressed && key == Qt::Key_F4) {
				if (isSpecialModifiersPressed) {
					m->last_keyboard_modifier = Qt::NoModifier;
					setFullScreen(false);
					close();
				}
			} else if (key == Qt::Key_CapsLock) {
				if (pressed && isSpecialModifiersPressed) {
					ui->widget_view->toggleCapsLock();
					return true;
				}
			} else if (e->nativeScanCode() == XK_TLDE) {
				if (pressed) {
					if (!m->tlde) {
						m->tlde = true;
						MyView::Key key;
						key.vk = VK_LMENU;
						key.pressed = true;
						ui->widget_view->sendRdpKeyboardEvent(key);
						return true;
					}
				} else {
					if (m->tlde) {
						m->tlde = false;
						MyView::Key key;
						key.vk = VK_LMENU;
						key.pressed = false;
						ui->widget_view->sendRdpKeyboardEvent(key);
						return true;
					}
				}
				return true;
			} else if (e->nativeScanCode() == XK_1) {
				if (m->tlde) {
					if (pressed) {
						MyView::Key key;
						key.vk = VK_LMENU;
						key.pressed = false;
						ui->widget_view->sendRdpKeyboardEvent(key);
						ui->widget_view->addKeyChunk();
						ui->widget_view->addKey(VK_OEM_3, true);
						ui->widget_view->addKeyChunk();
						ui->widget_view->addKey(VK_OEM_3, false);
						ui->widget_view->addKeyChunk();
						ui->widget_view->addKey(VK_LMENU, true);
						return true;
					}
				}
			} else if (key == Qt::Key_Alt || key == Qt::Key_Shift || key == Qt::Key_Control) {
				if (m->last_keyboard_modifier == Qt::NoModifier) {
					ui->widget_view->sendKeyboardModifiers(Qt::NoModifier);
				}
				if (key == Qt::Key_Alt) return true;
			} else {
				if (pressed && isSpecialModifiersPressed) {
					auto native = e->nativeScanCode();
					ui->widget_view->sendKeyboardModifiers(Qt::ControlModifier | Qt::AltModifier);
					ui->widget_view->addKeyChunk();
					ui->widget_view->addNativeKey(native, true);
					ui->widget_view->addKeyChunk();
					ui->widget_view->addNativeKey(native, false);
					return true;
				}
			}
			if (ui->widget_view->onKeyEvent(e)) return true;
		}
	}
#if 0
	if (event->type() == QEvent::ShortcutOverride) {
		qDebug() << event->type() << "ShortcutOverride";
		event->accept();
		return true;
	}
#endif
	return false;
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
	QMainWindow::resizeEvent(event);
	ui->widget_view->layoutView(true);
	resizeDynamicLater();
}

void MainWindow::on_action_connect_triggered()
{
	MySettings settings;
	settings.beginGroup("Connection");

	ConnectionDialog::Credential cred;
	cred.hostname = settings.value("Hostname", QString()).toString();
	cred.username = settings.value("Username", QString()).toString();
	cred.password = {};
	cred.domain = settings.value("Domain", "WORKGROUP").toString();

	ConnectionDialog dlg;
	dlg.setCredential(cred);
	if (dlg.exec() == QDialog::Accepted) {
		QString hostname = dlg.hostname();
		QString username = dlg.username();
		QString password = dlg.password();
		QString domain = dlg.domain();
		settings.setValue("Hostname", hostname);
		settings.setValue("Username", username);
		settings.setValue("Domain", domain);
		doConnect(hostname, username, password, domain);
		return;
	}
}

void MainWindow::on_action_disconnect_triggered()
{
	doDisconnect();
}

void MainWindow::start_rdp_thread()
{
	m->rdp_thread = std::thread([this]() {
		while (true) {
			if (m->interrupted) break;
			if (rdp_instance() && m->connected) {
				if (freerdp_shall_disconnect_context(m->session->rdp_context())) {
					emit emitDisconnect();
					break;
				}
				// イベント処理
				HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};
				int count = freerdp_get_event_handles(rdp_instance()->context, handles, MAXIMUM_WAIT_OBJECTS);
				auto r = WaitForMultipleObjects(count, handles, FALSE, 1);
				if (r == WAIT_FAILED) break;
				if (!freerdp_check_event_handles(rdp_instance()->context)) break;
				if (rdp_session_version() == RdpSessionVersion::V1) {
					QImage new_image;
					if (m->screen_image.isNull()) {
						auto *gdi = rdp_gdi();
						if (gdi->primary_buffer) {
							BYTE *data = gdi->primary_buffer;
							int width = gdi->width;
							int height = gdi->height;
							int stride = gdi->stride;
							new_image = QImage(data, width, height, stride, m->screen_image_foramt);
						}
					}
					if (!new_image.isNull()) {
						m->screen_image = new_image;
						emit requestUpdateScreen();
					}
				}
			}
		}
	});
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	if (m->last_keyboard_modifier & Qt::AltModifier) {
		ui->widget_view->sendKeyboardModifiers(Qt::AltModifier);
		ui->widget_view->addKeyChunk();
		ui->widget_view->addKey(VK_F4, true);
		ui->widget_view->addKeyChunk();
		ui->widget_view->addKey(VK_F4, false);
		ui->widget_view->sendKeyboardModifiers(Qt::NoModifier);
		m->last_keyboard_modifier = Qt::NoModifier;
		event->ignore();
		return;
	}

	if (isFullScreen()) {
		event->ignore();
		return;
	}

	if (m->connected) {
		if (QMessageBox::question(this, "Confirm Disconnect", "Are you sure you want to close Remote Desktop Client?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
			event->ignore();
			return;
		}
	}

	doDisconnect();

	{
		MySettings settings;
		setWindowOpacity(0);
		Qt::WindowStates state = windowState();
		bool maximized = (state & Qt::WindowMaximized) != 0;
		if (maximized) {
			state &= ~Qt::WindowMaximized;
			setWindowState(state);
		}
		{
			settings.beginGroup("MainWindow");
			settings.setValue("Maximized", maximized);
			settings.setValue("Geometry", saveGeometry());
			settings.endGroup();
		}
	}

	QMainWindow::closeEvent(event);
}

// FreeRDPコールバック関数の実装
BOOL MainWindow::rdp_pre_connect(freerdp *rdp)
{
	auto ctx = rdp->context;
	int r = 0;
	r = PubSub_SubscribeChannelConnected(ctx->pubSub, channelConnected);
	r = PubSub_SubscribeChannelDisconnected(ctx->pubSub, channelDisconnected);
	return TRUE;
}

BOOL MainWindow::onRdpPostConnect(freerdp *rdp)
{
	if (rdp_session_version() == RdpSessionVersion::V1) {
		if (!gdi_init(rdp, m->rdp_pixel_format)) {
			return FALSE;
		}
	} else if (rdp_session_version() == RdpSessionVersion::V2) {
		m->screen_image = QImage(m->size.width(), m->size.height(), m->screen_image_foramt);
		if (!gdi_init_ex(rdp, m->rdp_pixel_format, m->screen_image.bytesPerLine(), m->screen_image.bits(), nullptr)) {
			return FALSE;
		}
		if (isDynamicResizingEnabled()) {
			resizeDynamicLater();
		}
		setupRdpContext(rdp->context);
	}
	return TRUE;
}

BOOL MainWindow::rdp_post_connect(freerdp *instance)
{
	if (global->mainwindow) {
		return global->mainwindow->onRdpPostConnect(instance);
	}
	return FALSE;
}

void MainWindow::rdp_post_disconnect(freerdp *instance)
{
	gdi_free(instance);
}

BOOL MainWindow::rdp_authenticate(freerdp *instance, char **username, char **password, char **domain)
{
	(void)instance;
	(void)username;
	(void)password;
	(void)domain;
	return TRUE;
}

DWORD MainWindow::verifyCertificateEx(freerdp *rdp,
								const char *host,
								UINT16 port,
								const char *common_name,
								const char *subject,
								const char *issuer,
								const char *fingerprint,
								DWORD flags)
{
	using namespace rdpcert;

	Certificate cert;
	cert.host = host;
	cert.port = port;
	cert.commonName = common_name;
	cert.subject = subject;
	cert.issuer = issuer;
	cert.fingerprint = fingerprint;
	cert.flags = flags;

	CertResult ret = CertResult::Reject;

	VerifyCertificateDialog dlg(this);
	dlg.setNewCertificate(cert);
	if (dlg.exec() == QDialog::Accepted) {
		ret = dlg.result();
	}

	return static_cast<int>(ret);
}

DWORD MainWindow::rdp_verify_certificate_ex(freerdp *rdp, const char *host, UINT16 port, const char *common_name, const char *subject, const char *issuer, const char *fingerprint, DWORD flags)
{
	qDebug() << Q_FUNC_INFO;
	if (global->mainwindow) {
		return global->mainwindow->verifyCertificateEx(rdp, host, port, common_name, subject, issuer, fingerprint, flags);
	}
	return 0;
}

DWORD MainWindow::onRdpVerifyChangeCertificateEx(freerdp *instance, const char *host, UINT16 port,
												 const char *common_name, const char *subject,
												 const char *issuer, const char *new_fingerprint,
												 const char *old_subject, const char *old_issuer,
												 const char *old_fingerprint, DWORD flags)
{
	using namespace rdpcert;

	Certificate old_vert;
	old_vert.host = host;
	old_vert.port = port;
	old_vert.commonName = common_name;
	old_vert.subject = old_subject;
	old_vert.issuer = old_issuer;
	old_vert.fingerprint = old_fingerprint;
	old_vert.flags = flags;

	Certificate new_cert;
	new_cert.host = old_vert.host;
	new_cert.port = old_vert.port;
	new_cert.commonName = old_vert.commonName;
	new_cert.subject = subject;
	new_cert.issuer = issuer;
	new_cert.fingerprint = new_fingerprint;
	new_cert.flags = flags;

	CertResult ret = CertResult::Reject;

	VerifyCertificateDialog dlg(this);
	dlg.setChangedCertificate(old_vert, new_cert);
	if (dlg.exec() == QDialog::Accepted) {
		ret = dlg.result();
	}

	return static_cast<int>(ret);
}

DWORD MainWindow::rdp_verify_change_certificate_ex(freerdp *instance, const char *host, UINT16 port,
												   const char *common_name, const char *subject,
												   const char *issuer, const char *new_fingerprint,
												   const char *old_subject, const char *old_issuer,
												   const char *old_fingerprint, DWORD flags)
{
	qDebug() << Q_FUNC_INFO;
	if (global->mainwindow) {
		return global->mainwindow->onRdpVerifyChangeCertificateEx(instance, host, port, common_name, subject, issuer, new_fingerprint, old_subject, old_issuer, old_fingerprint, flags);
	}
	return 0;
}

BOOL MainWindow::rdp_end_paint(rdpContext *context)
{
	// static int _ = 0;
	// qDebug() << Q_FUNC_INFO << ++_;
	MyClientContext *ctx = reinterpret_cast<MyClientContext *>(context);
	MainWindow *self = ctx->self;
	rdpGdi *gdi = self->rdp_gdi();
	if (!gdi || !gdi->primary) return FALSE;

	// MyView側が前回のフレームをまだ消費していない場合、ここで全画面コピーを
	// 行っても表示される前に上書きされて捨てられるだけなので、コピー自体を
	// スキップしてRDP処理スレッドを解放する。screen_imageは以後もGDIによって
	// 更新され続けるため、次にここへ来たときには最新の累積状態を取得できる。
	if (self->m->v2_paint_pending.exchange(true)) {
		return TRUE;
	}

	auto invalid = gdi->primary->hdc->hwnd->invalid;
	QRect rect(invalid->x, invalid->y, invalid->w, invalid->h);

#if 1
	QImage img = self->m->screen_image.copy();
#else
	QImage img(self->m->screen_image.width(), self->m->screen_image.height(), self->m->screen_image.format());
	memcpy(img.bits(), self->m->screen_image.bits(), img.bytesPerLine() * img.height());
#endif
	self->updateScreen2(img, rect);

	return TRUE;
}

BOOL MainWindow::rdp_resize_display(rdpContext *context)
{
	MyClientContext *ctx = reinterpret_cast<MyClientContext *>(context);
	MainWindow *self = ctx->self;
	rdpGdi *gdi = self->rdp_gdi();
	if (!gdi || !gdi->primary) return FALSE;



	return TRUE;
}


bool MainWindow::isDynamicResizingEnabled() const
{
	return ui->action_view_dynamic_resolution->isChecked();
}

void MainWindow::on_action_view_dynamic_resolution_toggled(bool arg1)
{
	(void)arg1;
	if (isDynamicResizingEnabled()) {
		resizeDynamicLater();
	}
}

void MainWindow::resizeDynamicLater()
{
	m->dynamic_resize_counter = isDynamicResizingEnabled() ? 50 : 0;
}

void MainWindow::resizeDynamic()
{
	if (m->interrupted) return;
	if (!m->connected) return;
	if (!rdp_instance()) return;
	if (isDynamicResizingEnabled()) {
		auto size = newSize();
		if (size != m->size) {
			m->size = size;
			auto *settings = rdp_settings();
			auto *disp = disp_client_context();
			if (settings && disp && disp->DisplayControlCaps) {
				DISPLAY_CONTROL_MONITOR_LAYOUT layout = { 0 };
				layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
				layout.Left = 0;
				layout.Top = 0;
				layout.Width = m->size.width();
				layout.Height = m->size.height();
				layout.PhysicalWidth = m->size.width();
				layout.PhysicalHeight = m->size.height();
				layout.Orientation = freerdp_settings_get_uint16(settings, FreeRDP_DesktopOrientation);
				layout.DesktopScaleFactor = freerdp_settings_get_uint32(settings, FreeRDP_DesktopScaleFactor);
				layout.DeviceScaleFactor = freerdp_settings_get_uint32(settings, FreeRDP_DeviceScaleFactor);

				disp->SendMonitorLayout(disp, 1, &layout);

				freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, m->size.width());
				freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, m->size.height());

				auto gdi = rdp_gdi();
				if (gdi) {
					if (rdp_session_version() == RdpSessionVersion::V1) {
						gdi_resize(gdi, m->size.width(), m->size.height());
					} else if (rdp_session_version() == RdpSessionVersion::V2) {
						m->screen_image = QImage(m->size, m->screen_image_foramt);
						gdi_resize_ex(gdi, m->size.width(), m->size.height(), m->screen_image.bytesPerLine(), m->rdp_pixel_format, m->screen_image.bits(), nullptr);
					}
				}
			}
		}
	}
	ui->widget_view->layoutView(true);
}

void MainWindow::channelConnected(void *context, const ChannelConnectedEventArgs *e)
{
	if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
		if (global->mainwindow) {
			auto *self = global->mainwindow;
			auto *cliprdr = reinterpret_cast<CliprdrClientContext *>(e->pInterface);
			self->m->cliprdr = cliprdr;
			cliprdr->custom = self;
			cliprdr->MonitorReady = cliprdrMonitorReady;
			cliprdr->ServerFormatList = cliprdrServerFormatList;
			cliprdr->ServerFormatDataRequest = cliprdrServerFormatDataRequest;
			cliprdr->ServerFormatDataResponse = cliprdrServerFormatDataResponse;
		}
	} else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
		// contextをMyClientContext*として読み書きできるのは、V2(freerdp_client_context_new)で
		// ContextSize=sizeof(MyClientContext)として確保された場合のみ。V1(freerdp_context_new)の
		// contextは素のrdpContextでこれより小さいため、キャストして触れると領域外アクセスになる。
		if (global->mainwindow && global->mainwindow->rdp_session_version() == RdpSessionVersion::V2) {
			MyClientContext *ctx = reinterpret_cast<MyClientContext *>(context);
			ctx->disp = reinterpret_cast<DispClientContext *>(e->pInterface);
			ctx->disp->DisplayControlCaps = onDisplayControlCaps;
			ctx->disp->custom = reinterpret_cast<void *>(ctx->self);
		}
	} else {
		freerdp_client_OnChannelConnectedEventHandler(context, e);
	}
}

void MainWindow::channelDisconnected(void *context, const ChannelDisconnectedEventArgs *e)
{
	if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
		if (global->mainwindow) {
			auto *self = global->mainwindow;
			if (self->m->cliprdr) {
				self->m->cliprdr->custom = nullptr;
			}
			self->m->cliprdr = nullptr;
			self->m->requested_clipboard_format = 0;
			self->m->remote_clipboard_generation++;
			self->m->remote_clipboard_request_attempts = 0;
		}
	} else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
		if (global->mainwindow && global->mainwindow->rdp_session_version() == RdpSessionVersion::V2) {
			MyClientContext *ctx = reinterpret_cast<MyClientContext *>(context);
			ctx->disp->custom = nullptr;
			ctx->disp = nullptr;
		}
	} else {
		freerdp_client_OnChannelDisconnectedEventHandler(context, e);
	}
}

void MainWindow::sendClipboardFormatList()
{
	auto *cliprdr = m->cliprdr;
	if (!cliprdr || !cliprdr->ClientFormatList) return;

	const QMimeData *mime = QApplication::clipboard()->mimeData();
	m->local_clipboard_has_text = mime && mime->hasText();
	m->local_clipboard_has_image = mime && mime->hasImage();
	m->local_clipboard_text = m->local_clipboard_has_text ? mime->text() : QString();
	m->local_clipboard_image = m->local_clipboard_has_image
		? QApplication::clipboard()->image() : QImage();
	m->local_clipboard_has_image = !m->local_clipboard_image.isNull();

	CLIPRDR_FORMAT formats[2] = {};
	UINT32 count = 0;
	// 画像編集アプリが列挙順を優先度として扱う場合に備え、画像を先に提示する。
	if (m->local_clipboard_has_image) formats[count++].formatId = CF_DIB;
	if (m->local_clipboard_has_text) formats[count++].formatId = CF_UNICODETEXT;
	if (count == 0) return;
	CLIPRDR_FORMAT_LIST list = {};
	list.numFormats = count;
	list.formats = formats;
	cliprdr->ClientFormatList(cliprdr, &list);
}

UINT MainWindow::cliprdrMonitorReady(CliprdrClientContext *cliprdr, const CLIPRDR_MONITOR_READY *monitorReady)
{
	Q_UNUSED(monitorReady);
	if (cliprdr->ClientCapabilities) {
		CLIPRDR_GENERAL_CAPABILITY_SET general = {};
		general.capabilitySetType = CB_CAPSTYPE_GENERAL;
		general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
		general.version = CB_CAPS_VERSION_2;
		general.generalFlags = CB_USE_LONG_FORMAT_NAMES;

		CLIPRDR_CAPABILITIES capabilities = {};
		capabilities.cCapabilitiesSets = 1;
		capabilities.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET *>(&general);
		const UINT status = cliprdr->ClientCapabilities(cliprdr, &capabilities);
		if (status != CHANNEL_RC_OK) return status;
	}
	auto *self = static_cast<MainWindow *>(cliprdr->custom);
	if (self) {
		QMetaObject::invokeMethod(self, [self]() { self->sendClipboardFormatList(); }, Qt::QueuedConnection);
	}
	return CHANNEL_RC_OK;
}

UINT MainWindow::cliprdrServerFormatList(CliprdrClientContext *cliprdr, const CLIPRDR_FORMAT_LIST *formatList)
{
	CLIPRDR_FORMAT_LIST_RESPONSE response = {};
	response.common.msgFlags = CB_RESPONSE_OK;
	if (cliprdr->ClientFormatListResponse) {
		const UINT status = cliprdr->ClientFormatListResponse(cliprdr, &response);
		if (status != CHANNEL_RC_OK) return status;
	}

	bool hasUnicodeText = false;
	bool hasDib = false;
	for (UINT32 i = 0; i < formatList->numFormats; ++i) {
		hasUnicodeText |= formatList->formats[i].formatId == CF_UNICODETEXT;
		hasDib |= formatList->formats[i].formatId == CF_DIB;
	}
	const UINT32 requestedFormat = hasDib ? CF_DIB : (hasUnicodeText ? CF_UNICODETEXT : 0);
	if (requestedFormat != 0 && cliprdr->ClientFormatDataRequest) {
		auto *self = static_cast<MainWindow *>(cliprdr->custom);
		if (self) {
			QMetaObject::invokeMethod(self, [self, cliprdr, requestedFormat]() {
				self->beginRemoteClipboardRequest(cliprdr, requestedFormat);
			}, Qt::QueuedConnection);
		}
	}
	return CHANNEL_RC_OK;
}

void MainWindow::beginRemoteClipboardRequest(CliprdrClientContext *cliprdr, UINT32 format)
{
	if (m->cliprdr != cliprdr) return;
	const quint64 generation = ++m->remote_clipboard_generation;
	m->remote_clipboard_request_attempts = 0;
	QTimer::singleShot(30, this, [this, cliprdr, generation, format]() {
		requestRemoteClipboardData(cliprdr, generation, format);
	});
}

void MainWindow::requestRemoteClipboardData(CliprdrClientContext *cliprdr, quint64 generation, UINT32 format)
{
	if (m->cliprdr != cliprdr || m->remote_clipboard_generation != generation ||
		!cliprdr->ClientFormatDataRequest || m->remote_clipboard_request_attempts >= 5) {
		return;
	}

	m->remote_clipboard_request_attempts++;
	m->requested_clipboard_format = format;
	CLIPRDR_FORMAT_DATA_REQUEST request = {};
	request.requestedFormatId = format;
	const UINT status = cliprdr->ClientFormatDataRequest(cliprdr, &request);
	if (status != CHANNEL_RC_OK) {
		m->requested_clipboard_format = 0;
	}
}

UINT MainWindow::cliprdrServerFormatDataRequest(CliprdrClientContext *cliprdr, const CLIPRDR_FORMAT_DATA_REQUEST *request)
{
	if (!cliprdr->ClientFormatDataResponse) return CHANNEL_RC_OK;

	QString text;
	QImage image;
	auto *self = static_cast<MainWindow *>(cliprdr->custom);
	if (self) {
		auto readClipboard = [self, &text, &image]() {
			text = self->m->local_clipboard_text;
			image = self->m->local_clipboard_image;
		};
		if (QThread::currentThread() == self->thread()) {
			readClipboard();
		} else {
			QMetaObject::invokeMethod(self, readClipboard, Qt::BlockingQueuedConnection);
		}
	}

	QByteArray encoded;
	CLIPRDR_FORMAT_DATA_RESPONSE response = {};
	if (request->requestedFormatId == CF_UNICODETEXT) {
		encoded.resize((text.size() + 1) * 2);
		auto *dst = reinterpret_cast<uchar *>(encoded.data());
		for (qsizetype i = 0; i < text.size(); ++i) {
			qToLittleEndian<quint16>(text.utf16()[i], dst + i * 2);
		}
		qToLittleEndian<quint16>(0, dst + text.size() * 2);
		response.common.msgFlags = CB_RESPONSE_OK;
		response.common.dataLen = encoded.size();
		response.requestedFormatData = reinterpret_cast<const BYTE *>(encoded.constData());
	} else if (request->requestedFormatId == CF_DIB) {
		encoded = imageToDib(image);
		if (!encoded.isEmpty()) {
			response.common.msgFlags = CB_RESPONSE_OK;
			response.common.dataLen = encoded.size();
			response.requestedFormatData = reinterpret_cast<const BYTE *>(encoded.constData());
		} else {
			response.common.msgFlags = CB_RESPONSE_FAIL;
		}
	} else {
		response.common.msgFlags = CB_RESPONSE_FAIL;
	}
	return cliprdr->ClientFormatDataResponse(cliprdr, &response);
}

UINT MainWindow::cliprdrServerFormatDataResponse(CliprdrClientContext *cliprdr, const CLIPRDR_FORMAT_DATA_RESPONSE *response)
{
	auto *self = static_cast<MainWindow *>(cliprdr->custom);
	const UINT32 requestedFormat = self ? self->m->requested_clipboard_format.exchange(0) : 0;
	if (!self || (requestedFormat != CF_UNICODETEXT && requestedFormat != CF_DIB)) {
		return CHANNEL_RC_OK;
	}
	if (response->common.msgFlags & CB_RESPONSE_FAIL) {
		const quint64 generation = self->m->remote_clipboard_generation;
		QMetaObject::invokeMethod(self, [self, cliprdr, generation, requestedFormat]() {
			if (self->m->cliprdr != cliprdr || self->m->remote_clipboard_generation != generation)
				return;
			const int delay = 50 * self->m->remote_clipboard_request_attempts;
			QTimer::singleShot(delay, self, [self, cliprdr, generation, requestedFormat]() {
				self->requestRemoteClipboardData(cliprdr, generation, requestedFormat);
			});
		}, Qt::QueuedConnection);
		return CHANNEL_RC_OK;
	}
	QByteArray data(reinterpret_cast<const char *>(response->requestedFormatData), response->common.dataLen);
	QMetaObject::invokeMethod(self, [self, data, requestedFormat]() {
		if (requestedFormat == CF_DIB)
			self->setClipboardImageFromRdp(data);
		else
			self->setClipboardTextFromRdp(data);
	}, Qt::QueuedConnection);
	return CHANNEL_RC_OK;
}

void MainWindow::setClipboardTextFromRdp(const QByteArray &data)
{
	const qsizetype units = data.size() / 2;
	QString text;
	text.reserve(units);
	const auto *src = reinterpret_cast<const uchar *>(data.constData());
	for (qsizetype i = 0; i < units; ++i) {
		const quint16 ch = qFromLittleEndian<quint16>(src + i * 2);
		if (ch == 0) break;
		text.append(QChar(ch));
	}
	auto *mime = new QMimeData;
	mime->setText(text);
	mime->setData(REMOTE_CLIPBOARD_MIME, QByteArrayLiteral("1"));
	auto *clipboard = QApplication::clipboard();
	m->updating_remote_clipboard = true;
	clipboard->clear(QClipboard::Clipboard);
	clipboard->setMimeData(mime, QClipboard::Clipboard);
	m->updating_remote_clipboard = false;
}

void MainWindow::setClipboardImageFromRdp(const QByteArray &data)
{
	QImage image = dibToImage(data);
	if (image.isNull()) return;
	auto *mime = new QMimeData;
	mime->setImageData(image);
	mime->setData(REMOTE_CLIPBOARD_MIME, QByteArrayLiteral("1"));
	auto *clipboard = QApplication::clipboard();
	m->updating_remote_clipboard = true;
	clipboard->clear(QClipboard::Clipboard);
	clipboard->setMimeData(mime, QClipboard::Clipboard);
	m->updating_remote_clipboard = false;
}

UINT MainWindow::onDisplayControlCaps(DispClientContext *disp, UINT32 maxNumMonitors, UINT32 maxMonitorAreaFactorA, UINT32 maxMonitorAreaFactorB)
{
	return CHANNEL_RC_OK;
}

void MainWindow::on_action_full_screen_triggered()
{
	setFullScreen(true);
}

void MainWindow::on_action_exit_full_screen_triggered()
{
	setFullScreen(false);
}
