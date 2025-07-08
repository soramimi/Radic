#ifndef MYVIEW_H
#define MYVIEW_H

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWidget>
#include <freerdp/freerdp.h>
#include <freerdp/input.h>
#include <type_traits>

class CommandForm;

class MyView : public QWidget {
	Q_OBJECT
public:
	struct Key {
		DWORD vk = VK_NONE;
		bool pressed = false;
		bool autorepeat = false;
		Key() = default;
		Key(DWORD vk, bool pressed, bool autorepeat = false)
			: vk(vk)
			, pressed(pressed)
			, autorepeat(autorepeat)
		{
		}
	};
private:
	struct Private;
	Private *m;

	UINT16 qtToRdpMouseButton(Qt::MouseButton button);
	void startThread();
	void stopThread();
	void notifyAll();
protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;  // マウスホイールイベント追加

public:
	explicit MyView(QWidget *parent = nullptr);
	~MyView();
	void setImage(const QImage &image, const QRect &rect);
	void setRdpInstance(freerdp *instance);

	int scale() const;
	void setScale(int scale);

	void layoutView(bool update_view);
	
	void showCommandForm(bool show);
	bool isCommandFormVisible() const;

	bool onKeyEvent(QKeyEvent *event);
	bool sendRdpKeyboardEvent(const Key &k);
	void sendKeyboardModifiers(Qt::KeyboardModifiers mod);
	void toggleCapsLock();
	void addKeyChunk();
	void addKey(DWORD vk, bool press);
	void addNativeKey(quint32 native, bool pressed);
private:
	QPoint mapToRdp(const QPoint &pos) const;
	template <typename T> QPoint mapToRdp(T const *e) const
	{
		if constexpr (std::is_same_v<T, QWheelEvent>) {
			return mapToRdp(e->position().toPoint());
		} else {
			return mapToRdp(e->pos());
		}
	}
private slots:
	void kickUpdate();
public slots:
	bool sendKeyChunk();
signals:
	void ready();
};

#endif // MYVIEW_H
