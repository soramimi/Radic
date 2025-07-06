#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "ConnectionDialog.h"
#include "MySettings.h"
#include <QPainter>
#include <QWindow>
#include <thread>
#include "Global.h"
#include "VerifyCertificateDialog.h"
#include "rdpcert.h"

struct MyClientContext {
	rdpClientContext rdpcc;
	MainWindow *self = nullptr;
	DispClientContext *disp = nullptr;
};

class Session {
public:
	enum Version {
		V1,
		V2,
	};
#if 0
	Version version() { return V1; }

	freerdp *rdp = nullptr;

	void context_new(MainWindow *self)
	{
		rdp = freerdp_new();
		freerdp_context_new(rdp);
	}

	void context_free()
	{
		freerdp_free(rdp);
		rdp = nullptr;
	}

	freerdp *rdp_instance()
	{
		return rdp;
	}

	s_disp_client_context *disp_client_context()
	{
		return nullptr;
	}
#else
	Version version() { return V2; }

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

	s_disp_client_context *disp_client_context()
	{
		return d.cc->disp;
	}
#endif
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

struct MainWindow::Private {
	Session session;
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

	bool capslock = false;
};

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

	setDefaultWindowTitle();

	qApp->installEventFilter(this);

	connect(&m->update_timer, &QTimer::timeout, this, &MainWindow::onIntervalTimer);
	m->update_timer.setInterval(10);
	m->update_timer.start();

	connect(this, &MainWindow::requestUpdateScreen, this, &MainWindow::updateScreen);

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

void MainWindow::setDefaultWindowTitle()
{
	QString title = "Radic - Remote Desktop Client";
	setWindowTitle(title);
}

freerdp *MainWindow::rdp_instance()
{
	return m->session.rdp_instance();
}

rdpSettings *MainWindow::rdp_settings()
{
	return m->session.rdp_settings();
}

s_disp_client_context *MainWindow::disp_client_context()
{
	return m->session.disp_client_context();
}

rdpGdi *MainWindow::rdp_gdi()
{
	return m->session.rdp_gdi();
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

	// 動的解像度が有効な場合は、現在のビューサイズに合わせる
	if (isDynamicResizingEnabled()) {
		m->size = newSize();
	}

	m->interrupted = false;

	m->session.context_new(this);

	if (m->session.version() == Session::V1) {
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

	if (m->session.version() == Session::V2) {
		// Display拡張を有効化（動的解像度変更のため）
		freerdp_settings_set_bool(settings, FreeRDP_SupportDisplayControl, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DynamicResolutionUpdate, TRUE);
	}

	// 安全なパフォーマンス最適化設定のみ適用
	freerdp_settings_set_bool(settings, FreeRDP_FastPathOutput, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_FastPathInput, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_SupportDynamicChannels, TRUE);

	freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, true);
	freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, true);
	freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, true);
	freerdp_settings_set_bool(settings, FreeRDP_GfxH264, true);
	freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, false);
	freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);

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
		m->session.context_free();
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
		m->session.context_free();
	}
	m->connected = false;
	statusBar()->showMessage("Disconnected");

	QImage image(m->size.width(), m->size.height(), m->screen_image_foramt);
	image.fill(Qt::black);
	m->screen_image = image;
	if (m->session.version() == Session::V2) {
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
		if (m->session.version() == Session::V1) {
			ui->widget_view->setImage(image, QRect{});
		}
	}
}

void MainWindow::updateScreen2(QImage const &image, QRect const &rect)
{
	if (m->interrupted) return;
	if (!m->connected) return;

	if (!image.isNull()) {
		if (m->session.version() == Session::V2) {
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
			// qDebug() << Q_FUNC_INFO << pressed << QString::asprintf("%08x", key) << mod << e->nativeScanCode();
			bool isSpecialModifiersPressed = (pressed && alt && ctrl && shift);
			if (mod != m->last_keyboard_modifier) {
				m->last_keyboard_modifier = mod;
			}
			if (!pressed && !(key == Qt::Key_CapsLock || key == Qt::Key_Eisu_toggle)) {
				if (m->capslock) {
					m->capslock = false;
					MyView::Key key;
					key.vk = VK_LMENU;
					key.pressed = false;
					ui->widget_view->sendRdpKeyboardEvent(key);
					return true;
				}
			}
			if (pressed &&  key == Qt::Key_Backspace) {
				if (isSpecialModifiersPressed) {
					showCommandForm(!ui->widget_view->isCommandFormVisible());
					return true;
				}
			} else if (pressed && key == Qt::Key_F) {
				if (isSpecialModifiersPressed) {
					setFullScreen(!isFullScreen());
					return true;
				}
			} else if (pressed && key == Qt::Key_D) {
				if (isSpecialModifiersPressed) {
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
			} else if (key == Qt::Key_CapsLock || key == Qt::Key_Eisu_toggle) {
				if (isSpecialModifiersPressed) {
					if (pressed) {
						ui->widget_view->toggleCapsLock();
						return true;
					}
				} else {
					if (pressed) {
						if (!m->capslock) {
							m->capslock = true;
							MyView::Key key;
							key.vk = VK_LMENU;
							key.pressed = true;
							ui->widget_view->sendRdpKeyboardEvent(key);
							return true;
						}
					}
				}
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
				// イベント処理
				HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};
				int count = freerdp_get_event_handles(rdp_instance()->context, handles, MAXIMUM_WAIT_OBJECTS);
				auto r = WaitForMultipleObjects(count, handles, FALSE, 1);
				if (r == WAIT_FAILED) break;
				if (!freerdp_check_event_handles(rdp_instance()->context)) break;
				if (m->session.version() == Session::V1) {
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
				// static int _ = 0;
				// qDebug() << count << ++_;
				// freerdp_check_fds(rdp_instance());
			}
		}
	});
}

void MainWindow::closeEvent(QCloseEvent *event)
{
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
	if (m->session.version() == Session::V1) {
		if (!gdi_init(rdp, m->rdp_pixel_format)) {
			return FALSE;
		}
	} else if (m->session.version() == Session::V2) {
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
	dlg.setNewCertificate(new_cert);
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
					if (m->session.version() == Session::V1) {
						gdi_resize(gdi, m->size.width(), m->size.height());
					} else if (m->session.version() == Session::V2) {
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
	} else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
		MyClientContext *ctx = reinterpret_cast<MyClientContext *>(context);
		ctx->disp = reinterpret_cast<DispClientContext *>(e->pInterface);
		ctx->disp->DisplayControlCaps = onDisplayControlCaps;
		ctx->disp->custom = reinterpret_cast<void *>(ctx->self);
	} else {
		freerdp_client_OnChannelConnectedEventHandler(context, e);
	}
}

void MainWindow::channelDisconnected(void *context, const ChannelDisconnectedEventArgs *e)
{
	if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
	} else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
		MyClientContext *ctx = reinterpret_cast<MyClientContext *>(context);
		ctx->disp->custom = nullptr;
		ctx->disp = nullptr;
	} else {
		freerdp_client_OnChannelDisconnectedEventHandler(context, e);
	}
}

UINT MainWindow::onDisplayControlCaps(DispClientContext *disp, UINT32 maxNumMonitors, UINT32 maxMonitorAreaFactorA, UINT32 maxMonitorAreaFactorB)
{
	return CHANNEL_RC_OK;
}

