/*
 * ty, a collection of GUI and command-line tools to manage Teensy devices
 *
 * Distributed under the MIT license (see LICENSE.txt or http://opensource.org/licenses/MIT)
 * Copyright (c) 2015 Niels Martignène <niels.martignene@gmail.com>
 */

#ifndef ABOUT_HH
#define ABOUT_HH

#include "ui_about_dialog.h"

class AboutDialog: public QDialog, private Ui::AboutDialog {
    Q_OBJECT

public:
    AboutDialog(QWidget *parent = nullptr, Qt::WindowFlags f = 0);

private slots:
    void on_websiteButton_clicked();
    void on_licenseButton_clicked();
    void on_descriptionText_linkActivated(const QString &link);
};

#endif