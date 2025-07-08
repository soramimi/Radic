#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QDebug>
#include <QImage>
#include <QInputDialog>
#include <QMainWindow>
#include <QMessageBox>
#include <QTimer>
#include <freerdp/channels/channels.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/primary.h>
#include <thread>
#include <freerdp/freerdp.h>
#include <freerdp/client/disp.h>
#include <freerdp/client/cliprdr.h>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

// class s_disp_client_context;

class MainWindow : public QMainWindow {
	Q_OBJECT
	friend class CommandForm;
	friend class Session;
private:
	struct Private;
	struct Private *m;
	Ui::MainWindow *ui;
	
	// FreeRDPコールバック関数
	static BOOL rdp_pre_connect(freerdp *instance);
	static BOOL rdp_post_connect(freerdp *instance);
	static void rdp_post_disconnect(freerdp *instance);
	static BOOL rdp_authenticate(freerdp *instance, char **username, char **password, char **domain);
	static BOOL rdp_end_paint(rdpContext *context);
	static BOOL rdp_resize_display(rdpContext *context);

	void doConnect(const QString &hostname, const QString &username, const QString &password, const QString &domain);
	void doDisconnect();
	BOOL onRdpPostConnect(freerdp *instance);
	void start_rdp_thread();
	void resizeDynamic();
	void resizeDynamicLater();
	static void channelConnected(void *context, const ChannelConnectedEventArgs *e);
	static void channelDisconnected(void *context, const ChannelDisconnectedEventArgs *e);
	rdpContext *rdp_context();
	freerdp *rdp_instance();
	s_disp_client_context *disp_client_context();
	static UINT onDisplayControlCaps(DispClientContext *disp, UINT32 maxNumMonitors, UINT32 maxMonitorAreaFactorA, UINT32 maxMonitorAreaFactorB);
	rdpGdi *rdp_gdi();
	rdpSettings *rdp_settings();
	QSize newSize() const;
	void setDefaultWindowTitle();
	DWORD verifyCertificateEx(freerdp *rdp, const char *host, UINT16 port, const char *common_name, const char *subject, const char *issuer, const char *fingerprint, DWORD flags);
	static DWORD rdp_verify_certificate_ex(freerdp *rdp, const char *host, UINT16 port, const char *common_name, const char *subject, const char *issuer, const char *fingerprint, DWORD flags);
	DWORD onRdpVerifyChangeCertificateEx(freerdp *instance, const char *host, UINT16 port, const char *common_name, const char *subject, const char *issuer, const char *new_fingerprint, const char *old_subject, const char *old_issuer, const char *old_fingerprint, DWORD flags);
	static DWORD rdp_verify_change_certificate_ex(freerdp *instance, const char *host, UINT16 port, const char *common_name, const char *subject, const char *issuer, const char *new_fingerprint, const char *old_subject, const char *old_issuer, const char *old_fingerprint, DWORD flags);
	static BOOL clientGlobalInit();
	static void clientGlobalUninit();
	static BOOL clientContextNew(freerdp *instance, rdpContext *context);
	static void clientContextFree(freerdp *instance, rdpContext *context);
	static int clientContextStart(rdpContext *context);
	static int clientContextStop(rdpContext *context);
	void initInstance(freerdp *instance);
	void setupRdpContext(rdpContext *rdpcx);
protected:
	void closeEvent(QCloseEvent *event);
public:
	MainWindow(QWidget *parent = nullptr);
	virtual ~MainWindow();
private slots:
	void on_action_connect_triggered();
	void on_action_disconnect_triggered();
	void updateScreen();
	void updateScreen2(const QImage &image, const QRect &rect);
	void on_action_view_dynamic_resolution_toggled(bool arg1);

signals:
	void requestUpdateScreen();
	void emitConnect();

	// QObject interface
public:
	bool eventFilter(QObject *watched, QEvent *event);

	bool isDynamicResizingEnabled() const;
	void setFullScreen(bool full_screen);
	void showCommandForm(bool show);
private slots:
	void onIntervalTimer();
	void on_action_full_screen_triggered();
	void on_action_exit_full_screen_triggered();
protected:
	void resizeEvent(QResizeEvent *event);
};
#endif // MAINWINDOW_H
