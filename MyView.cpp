

#include "CommandForm.h"
#include "MyView.h"
#include <QApplication>
#include <QPainter>
#include <QWheelEvent>
#include <freerdp/scancode.h>
#include <mutex>
#include <thread>
#include <condition_variable>

struct MyView::Private {
	CommandForm *command_form = nullptr;

	freerdp *rdp_instance = nullptr;

	std::mutex mutex;
	std::thread thread;
	std::condition_variable cv;
	bool interrupted = false;

	QSize frame_size;

	QRect update_rect;
	QImage next_input_frame;
	QImage next_output_frame;
	QImage scaled_image;

	int scale = 1;
	int offset_x = 0;
	int offset_y = 0;

};

MyView::MyView(QWidget *parent)
	: QWidget { parent }
	, m(new Private)
{
	m->command_form = new CommandForm(this);
	m->command_form->hide();

	setFocusPolicy(Qt::StrongFocus);
	setMouseTracking(true);

	connect(this, &MyView::ready, this, &MyView::kickUpdate);
	startThread();
}

MyView::~MyView()
{
	stopThread();
	delete m;
}

void MyView::startThread()
{
	m->interrupted = false;
	m->thread = std::thread([this]() {
		while (true) {
			if (m->interrupted) break; // 中断フラグが立っている場合はループを終了
			QImage next_input_frame;
			{
				std::unique_lock<std::mutex> lock(m->mutex);
				m->cv.wait(lock);
				if (m->interrupted) break;
				std::swap(next_input_frame, m->next_input_frame);
			}
			if (!m->rdp_instance) continue;
			if (!m->rdp_instance->context) continue;
			if (next_input_frame.isNull()) continue;
			QRect update_rect;
			{
				std::lock_guard lock(m->mutex);
				update_rect = m->update_rect;
				if (update_rect.isNull() || m->next_output_frame.size() != next_input_frame.size()) {
					m->next_output_frame = QImage(next_input_frame.width(), next_input_frame.height(), next_input_frame.format());
					update_rect = next_input_frame.rect();
				}
			}
			{
				QPainter pr(&m->next_output_frame);
				pr.drawImage(update_rect, next_input_frame, update_rect);
			}
			{//if (m->next_output_frame.size() != m->scaled_image.size()) {
				std::lock_guard lock(m->mutex);
				if (m->scale == 1) {
					m->scaled_image = m->next_output_frame.copy();
				} else {
					int w = m->frame_size.width() * m->scale;
					int h = m->frame_size.height() * m->scale;
					m->scaled_image = m->next_output_frame.scaled(w, h, Qt::KeepAspectRatio, Qt::FastTransformation);
				}
			}
			emit ready();
		}
	});
}

void MyView::stopThread()
{
	m->interrupted = true;
	m->cv.notify_all(); // スレッドを起床させる
	if (m->thread.joinable()) {
		m->thread.join();
	}
}

void MyView::notifyAll()
{
	std::lock_guard lock(m->mutex);
	if (!m->interrupted) {
		m->cv.notify_all(); // スレッドを起床させる
	}
}

QPoint MyView::mapToRdp(const QPoint &pos) const
{
	// RDPの座標系に変換
	return QPoint((pos.x() + m->offset_x) / m->scale, (pos.y() + m->offset_y) / m->scale);
}

void MyView::kickUpdate()
{
	update();
}

void MyView::setImage(const QImage &image, QRect const &rect)
{
	{
		std::lock_guard lock(m->mutex);
		m->frame_size = image.size();
		m->next_input_frame = image;
		m->update_rect = rect;
	}
	m->cv.notify_all(); // スレッドを起床させる
	layoutView(false);
}

void MyView::layoutView(bool update_view)
{
	int w = m->frame_size.width() * m->scale;
	int h = m->frame_size.height() * m->scale;
	int x = (w > width()) ? 0 : (width() - w) / 2;
	int y = (h > height()) ? (height() - h) : (height() - h) / 2;
	m->offset_x = -x;
	m->offset_y = -y;

	if (update_view) {
		update();
	}
}

void MyView::showCommandForm(bool show)
{
	m->command_form->setVisible(show);
}

bool MyView::isCommandFormVisible() const
{
	return m->command_form->isVisible();
}

void MyView::setRdpInstance(freerdp *instance)
{
	m->rdp_instance = instance;
}

int MyView::scale() const
{
	return m->scale;
}

void MyView::setScale(int scale)
{
	m->scale = scale;
	layoutView(true);
}

void MyView::paintEvent(QPaintEvent *event)
{
	Q_UNUSED(event);
	std::lock_guard lock(m->mutex);
	QPainter painter(this);
	painter.fillRect(rect(), QColor(192, 192, 192));
	if (!m->scaled_image.isNull()) {
		int x = -m->offset_x;
		int y = -m->offset_y;
		int w = m->scaled_image.width();
		int h = m->scaled_image.height();
		{
			painter.fillRect(x - 1, y - 1, w + 2, h + 2, Qt::black);
			painter.fillRect(x - 2, y - 2, w + 2, 1, QColor(128, 128, 128));
			painter.fillRect(x - 2, y - 2, 1, h + 2, QColor(128, 128, 128));
			painter.fillRect(x, y + h + 1, w + 2, 1, QColor(255, 255, 255));
			painter.fillRect(x + w + 1, y, 1, h + 2, QColor(255, 255, 255));
		}
		painter.drawImage(x, y, m->scaled_image);
	} else {
		qDebug() << "!";
	}
}

void MyView::mousePressEvent(QMouseEvent *event)
{
	if (m->rdp_instance && m->rdp_instance->context) {
		UINT16 flags = PTR_FLAGS_DOWN;
		UINT16 button = qtToRdpMouseButton(event->button());
		if (button != 0) {
			flags |= button;
			QPoint pos = mapToRdp(event);
			freerdp_input_send_mouse_event(m->rdp_instance->context->input, flags, pos.x(), pos.y());
		}
	}
	setFocus();
}

void MyView::mouseReleaseEvent(QMouseEvent *event)
{
	if (m->rdp_instance && m->rdp_instance->context) {
		UINT16 button = qtToRdpMouseButton(event->button());
		if (button != 0) {
			QPoint pos = mapToRdp(event);
			freerdp_input_send_mouse_event(m->rdp_instance->context->input, button, pos.x(), pos.y());
		}
	}
}

void MyView::mouseMoveEvent(QMouseEvent *event)
{
	if (m->rdp_instance && m->rdp_instance->context) {
		QPoint pos = mapToRdp(event);
		freerdp_input_send_mouse_event(m->rdp_instance->context->input, PTR_FLAGS_MOVE, pos.x(), pos.y());
	}
}

void MyView::wheelEvent(QWheelEvent *event)
{
	if (m->rdp_instance && m->rdp_instance->context) {
		auto delta = event->angleDelta();
		QPoint pos = mapToRdp(event);
		if (delta.y() != 0) {
			// 垂直スクロール（一般的なマウスホイール）
			int flags = std::abs(delta.y());
			flags = std::clamp(flags, 0, 255);
			flags |= PTR_FLAGS_WHEEL;
			if (delta.y() < 0) {
				flags |= PTR_FLAGS_WHEEL_NEGATIVE;
			}
			// qDebug() << Q_FUNC_INFO << flags;
			freerdp_input_send_mouse_event(m->rdp_instance->context->input, (UINT16)flags, pos.x(), pos.y());
		} else if (delta.x() != 0) {
			// 水平スクロール（ホイールチルト）
			int flags = std::abs(delta.x());
			flags = std::clamp(flags, 0, 255);
			flags |= PTR_FLAGS_HWHEEL;
			if (delta.x() < 0) {
				flags |= PTR_FLAGS_WHEEL_NEGATIVE;  // 左スクロール
			}
			freerdp_input_send_mouse_event(m->rdp_instance->context->input, (UINT16)flags, pos.x(), pos.y());
		}
	}
}

bool MyView::onKeyEvent(QKeyEvent *event)
{
	if (m->rdp_instance && m->rdp_instance->context && m->rdp_instance->context->input) {
		auto vc = GetVirtualKeyCodeFromKeycode(event->nativeScanCode(), WINPR_KEYCODE_TYPE_XKB);
		auto code = GetVirtualScanCodeFromVirtualKeyCode(vc, WINPR_KBD_TYPE_IBM_ENHANCED);
		freerdp_input_send_keyboard_event_ex(m->rdp_instance->context->input, event->type() == QEvent::KeyPress, event->isAutoRepeat(), code);
		return true;
	}
	return false;
}

UINT16 MyView::qtToRdpMouseButton(Qt::MouseButton button)
{
	switch (button) {
	case Qt::LeftButton:
		return PTR_FLAGS_BUTTON1;
	case Qt::RightButton:
		return PTR_FLAGS_BUTTON2;
	case Qt::MiddleButton:
		return PTR_FLAGS_BUTTON3;
	default:
		return 0;
	}
}

