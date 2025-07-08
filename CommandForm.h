#ifndef COMMANDFORM_H
#define COMMANDFORM_H

#include <QFrame>
#include <QMainWindow>
#include <QWidget>

namespace Ui {
class CommandForm;
}

class CommandForm : public QMainWindow {
	Q_OBJECT
private:
	Ui::CommandForm *ui;
public:
	explicit CommandForm(QWidget *parent = nullptr);
	~CommandForm();


	// QWidget interface
protected:
	void paintEvent(QPaintEvent *event);
private slots:
	void on_action_disconnect_triggered();
	void on_action_exit_full_screen_triggered();
};

#endif // COMMANDFORM_H
