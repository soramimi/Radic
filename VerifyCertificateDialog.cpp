#include "VerifyCertificateDialog.h"
#include "ui_VerifyCertificateDialog.h"
#include <QString>

VerifyCertificateDialog::VerifyCertificateDialog(QWidget *parent)
	: QDialog(parent)
	, ui(new Ui::VerifyCertificateDialog)
	, result_(rdpcert::CertResult::Reject)
{
	ui->setupUi(this);
	setModal(true);
	
	// Set default button focus
	ui->pushButton_reject->setDefault(true);
	ui->pushButton_reject->setFocus();
	
	// Connect buttons to slots
	connect(ui->pushButton_reject, &QPushButton::clicked, this, &VerifyCertificateDialog::on_pushButton_reject_clicked);
	connect(ui->pushButton_accept_temp, &QPushButton::clicked, this, &VerifyCertificateDialog::on_pushButton_accept_temp_clicked);
	connect(ui->pushButton_accept_perm, &QPushButton::clicked, this, &VerifyCertificateDialog::on_pushButton_accept_perm_clicked);
}

VerifyCertificateDialog::~VerifyCertificateDialog()
{
	delete ui;
}

void VerifyCertificateDialog::setNewCertificate(const rdpcert::Certificate &cert)
{
	// Update UI with certificate information
	ui->label_host->setText(QString::fromStdString(cert.host));
	ui->label_port->setText(QString::number(cert.port));
	ui->label_common_name->setText(QString::fromStdString(cert.commonName));
	ui->label_subject->setText(QString::fromStdString(cert.subject));
	ui->label_issuer->setText(QString::fromStdString(cert.issuer));
	
	// Format fingerprint for better readability
	QString fingerprint = QString::fromStdString(cert.fingerprint);
	if (!fingerprint.isEmpty()) {
		// Add colons between pairs of characters for better readability
		QString formatted;
		for (int i = 0; i < fingerprint.length(); i += 2) {
			if (i > 0) formatted += ":";
			formatted += fingerprint.mid(i, 2);
		}
		ui->label_fingerprint->setText(formatted.toUpper());
	} else {
		ui->label_fingerprint->setText("-");
	}
	
	// Set window title with host information
	setWindowTitle(QString("Certificate Verification - %1:%2")
				   .arg(QString::fromStdString(cert.host))
				   .arg(cert.port));
}

rdpcert::CertResult VerifyCertificateDialog::result() const
{
	return result_;
}

void VerifyCertificateDialog::on_pushButton_reject_clicked()
{
	result_ = rdpcert::CertResult::Reject;
	reject();
}

void VerifyCertificateDialog::on_pushButton_accept_temp_clicked()
{
	result_ = rdpcert::CertResult::AcceptTemporarily;
	accept();
}

void VerifyCertificateDialog::on_pushButton_accept_perm_clicked()
{
	result_ = rdpcert::CertResult::AcceptPermanently;
	accept();
}
