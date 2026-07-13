#include "VerifyCertificateDialog.h"
#include "ui_VerifyCertificateDialog.h"
#include <QString>
#include <freerdp/freerdp.h>

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

QString VerifyCertificateDialog::formatFingerprint(const std::string &fingerprint)
{
	QString value = QString::fromStdString(fingerprint);
	if (value.isEmpty()) return "-";
	// Add colons between pairs of characters for better readability
	QString formatted;
	for (int i = 0; i < value.length(); i += 2) {
		if (i > 0) formatted += ":";
		formatted += value.mid(i, 2);
	}
	return formatted.toUpper();
}

void VerifyCertificateDialog::setCertificateDetails(const rdpcert::Certificate &cert)
{
	ui->label_host->setText(QString::fromStdString(cert.host));
	ui->label_port->setText(QString::number(cert.port));
	ui->label_common_name->setText(QString::fromStdString(cert.commonName));
	ui->label_subject->setText(QString::fromStdString(cert.subject));
	ui->label_issuer->setText(QString::fromStdString(cert.issuer));
	ui->label_fingerprint->setText(formatFingerprint(cert.fingerprint));

	setWindowTitle(QString("Certificate Verification - %1:%2")
				   .arg(QString::fromStdString(cert.host))
				   .arg(cert.port));
}

void VerifyCertificateDialog::setWarningForFlags(int flags)
{
	// VERIFY_CERT_FLAG_MISMATCH: hostname doesn't match the certificate - the
	// strongest indicator of a possible man-in-the-middle attack, so it takes
	// priority over the (milder) "changed since last time" case.
	if (flags & VERIFY_CERT_FLAG_MISMATCH) {
		ui->label_warning->setText(tr("<b>Warning: Certificate hostname mismatch</b>"));
		ui->label_description->setText(tr(
			"The certificate's hostname does not match the server you are connecting to. "
			"This is a strong indicator of a man-in-the-middle attack. Do not accept unless "
			"you are certain this is expected."));
	} else if (flags & VERIFY_CERT_FLAG_CHANGED) {
		ui->label_warning->setText(tr("<b>Warning: Certificate has changed</b>"));
		ui->label_description->setText(tr(
			"The certificate presented by this server is different from the one previously "
			"trusted for this host. This could mean the server was reconfigured, or it could "
			"indicate a man-in-the-middle attack. Compare the fingerprints below carefully "
			"before accepting."));
	} else {
		ui->label_warning->setText(tr("<b>Warning: Certificate verification failed</b>"));
		ui->label_description->setText(tr(
			"The server certificate could not be verified. Please review the certificate "
			"details below and decide whether to accept this certificate."));
	}
}

void VerifyCertificateDialog::setNewCertificate(const rdpcert::Certificate &cert)
{
	ui->groupBox_previous->setVisible(false);
	ui->groupBox->setTitle(tr("Certificate Details"));
	setWarningForFlags(cert.flags);
	setCertificateDetails(cert);
}

void VerifyCertificateDialog::setChangedCertificate(const rdpcert::Certificate &oldCert, const rdpcert::Certificate &newCert)
{
	ui->groupBox->setTitle(tr("New Certificate"));
	setWarningForFlags(newCert.flags | VERIFY_CERT_FLAG_CHANGED);
	setCertificateDetails(newCert);

	ui->label_old_subject->setText(QString::fromStdString(oldCert.subject));
	ui->label_old_issuer->setText(QString::fromStdString(oldCert.issuer));
	ui->label_old_fingerprint->setText(formatFingerprint(oldCert.fingerprint));
	ui->groupBox_previous->setVisible(true);
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
