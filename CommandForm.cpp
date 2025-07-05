#include "CommandForm.h"
#include "ui_CommandForm.h"
#include "Global.h"
#include <QPainter>
#include "MainWindow.h"

CommandForm::CommandForm(QWidget *parent)
	: QMainWindow(parent)
	, ui(new Ui::CommandForm)
{
	ui->setupUi(this);
	setWindowFlags(Qt::Widget);
	setGeometry(32, 24, 320, 240);
}

CommandForm::~CommandForm()
{
	delete ui;
}

void CommandForm::paintEvent(QPaintEvent *event)
{
	{
		QPainter pr(this);
		QColor color = palette().color(QPalette::Window);
		color.setAlpha(192);
		pr.fillRect(rect(), color);
	}
	QMainWindow::paintEvent(event);
}

void CommandForm::on_action_disconnect_triggered()
{
	global->mainwindow->on_action_disconnect_triggered();
}

