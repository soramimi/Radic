#ifndef VERIFYCERTIFICATEDIALOG_H
#define VERIFYCERTIFICATEDIALOG_H

#include <QDialog>
#include "rdpcert.h"

namespace Ui {
class VerifyCertificateDialog;
}

class VerifyCertificateDialog : public QDialog {
	Q_OBJECT
private:
	Ui::VerifyCertificateDialog *ui;
	rdpcert::CertResult result_;
public:
	explicit VerifyCertificateDialog(QWidget *parent = nullptr);
	~VerifyCertificateDialog();
	void setNewCertificate(const rdpcert::Certificate &cert);
	rdpcert::CertResult result() const;
private slots:
	void on_pushButton_reject_clicked();
	void on_pushButton_accept_temp_clicked();
	void on_pushButton_accept_perm_clicked();
};

#endif // VERIFYCERTIFICATEDIALOG_H
