#ifndef AUTHENFRIEND_H
#define AUTHENFRIEND_H

#include <QDialog>
#include <memory>

#include "userdata.h"

namespace Ui {
class AuthenFriend;
}

class AuthenFriend : public QDialog
{
    Q_OBJECT

public:
    explicit AuthenFriend(QWidget *parent = nullptr);
    ~AuthenFriend();

    void SetApplyInfo(std::shared_ptr<ApplyInfo> apply_info);

private slots:
    void SlotApplySure();
    void SlotApplyReject();
    void SlotApplyCancel();

private:
    Ui::AuthenFriend *ui;
    std::shared_ptr<ApplyInfo> _apply_info;
};

#endif // AUTHENFRIEND_H
