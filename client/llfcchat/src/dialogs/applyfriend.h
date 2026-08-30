#ifndef APPLYFRIEND_H
#define APPLYFRIEND_H

#include <QDialog>
#include <memory>

#include "userdata.h"

namespace Ui {
class ApplyFriend;
}

class ApplyFriend : public QDialog
{
    Q_OBJECT

public:
    explicit ApplyFriend(QWidget *parent = nullptr);
    ~ApplyFriend();

    void SetSearchInfo(std::shared_ptr<SearchInfo> search_info);

private slots:
    void SlotApplySure();
    void SlotApplyCancel();

private:
    Ui::ApplyFriend *ui;
    std::shared_ptr<SearchInfo> _search_info;
};

#endif // APPLYFRIEND_H
