

#include "CommandForm.h"
#include "MyView.h"
#include <QApplication>
#include <QPainter>
#include <QWheelEvent>
#include <freerdp/scancode.h>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <QTimer>
#include <deque>
#include <QPainterPath>

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
	QImage painting_image;

	int scale = 1;
	int offset_x = 0;
	int offset_y = 0;

	QTimer fps_timer;
	int frame_count = 0;
	int fps = 0;

	QTimer key_event_timer;
	std::deque<std::vector<Key>> key_event_queue;
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

	connect(&m->fps_timer, &QTimer::timeout, this, [this]() {
		m->fps = m->frame_count;
		m->frame_count = 0;
		update();
	});
	m->fps_timer.start(1000); // 1秒ごとにFPSを更新

	connect(&m->key_event_timer, &QTimer::timeout, this, &MyView::sendKeyChunk);
	m->key_event_timer.start(1);
}

MyView::~MyView()
{
	m->key_event_timer.stop();
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
			{
				std::lock_guard lock(m->mutex);
				m->painting_image = m->next_output_frame.copy();
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
	QPainter painter(this);
	QRect r;
	{
		std::lock_guard lock(m->mutex);
		if (!m->painting_image.isNull()) {
			int x = -m->offset_x;
			int y = -m->offset_y;
			int w = m->painting_image.width() * m->scale;
			int h = m->painting_image.height() * m->scale;
			r = {x, y, w, h};
			painter.drawImage(r, m->painting_image, m->painting_image.rect());
		}
	}
	{
		painter.save();
		int x = r.x();
		int y = r.y();
		int w = r.width();
		int h = r.height();
		QPainterPath path;
		QPainterPath imgrect;
		path.addRect(rect());
		imgrect.addRect(x, y, w, h);
		path = path.subtracted(imgrect);
		painter.setClipPath(path);
		painter.fillRect(rect(), QColor(192, 192, 192));
		painter.fillRect(x - 1, y - 1, w + 2, 1, Qt::black);
		painter.fillRect(x - 1, y - 1, 1, h + 2, Qt::black);
		painter.fillRect(x, y + h, w + 2, 1, Qt::black);
		painter.fillRect(x + w, y, 1, h + 2, Qt::black);
		painter.fillRect(x - 2, y - 2, w + 2, 1, QColor(128, 128, 128));
		painter.fillRect(x - 2, y - 2, 1, h + 2, QColor(128, 128, 128));
		painter.fillRect(x, y + h + 1, w + 2, 1, QColor(255, 255, 255));
		painter.fillRect(x + w + 1, y, 1, h + 2, QColor(255, 255, 255));
		painter.restore();
	}
	if (1) {
		painter.setPen(Qt::black);
		painter.setFont(QFont("Arial", 10));
		painter.drawText(10, 20, QString("FPS: %1").arg(m->fps));
	}
	m->frame_count++;
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

bool MyView::sendRdpKeyboardEvent(Key const &k)
{
	if (m->rdp_instance && m->rdp_instance->context && m->rdp_instance->context->input) {
		// qDebug() << k.vk << k.pressed;
		auto code = GetVirtualScanCodeFromVirtualKeyCode(k.vk, WINPR_KBD_TYPE_IBM_ENHANCED);
		freerdp_input_send_keyboard_event_ex(m->rdp_instance->context->input, k.pressed, k.autorepeat, code);
		return true;
	}
	return false;
}

bool MyView::sendKeyChunk()
{
	bool ret = false;
	std::vector<Key> keys;
	if (!m->key_event_queue.empty()) {
		keys = std::move(m->key_event_queue.front());
		m->key_event_queue.pop_front();
	}
	for (const auto &key : keys) {
		if (key.vk != VK_NONE) {
			ret = sendRdpKeyboardEvent(key);
		}
	}
	return ret;
}

void MyView::addKeyChunk()
{
	m->key_event_queue.push_back({});
}

void MyView::addKey(DWORD vk, bool press)
{
	m->key_event_queue.back().emplace_back(vk, press, false);
}

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

void MyView::toggleCapsLock()
{
	sendKeyboardModifiers(Qt::NoModifier);

	addKeyChunk();
	addKey(VK_LSHIFT, true);
	addKeyChunk();
	addKey(VK_CAPITAL, true);

	addKeyChunk();
	addKey(VK_CAPITAL, false);
	addKeyChunk();
	addKey(VK_LSHIFT, false);
}

void MyView::addNativeKey(quint32 native, bool pressed)
{
	auto vk = GetVirtualKeyCodeFromKeycode(native, WINPR_KEYCODE_TYPE_XKB);
	addKey(vk, pressed);
}

bool MyView::onKeyEvent(QKeyEvent *event)
{
	bool pressed = event->type() == QEvent::KeyPress;
	auto vk = GetVirtualKeyCodeFromKeycode(event->nativeScanCode(), WINPR_KEYCODE_TYPE_XKB);
	// return sendRdpKeyboardEvent({vk, pressed, event->isAutoRepeat()});
	addKeyChunk();
	addKey(vk, pressed);
	return sendKeyChunk();
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

