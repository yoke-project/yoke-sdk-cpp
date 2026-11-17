// The QML binding of the interface library: the objects a QML document instantiates to attach to a
// channel, read what the channel sees, and follow it.
//
// It wraps yoke::iface::Attachment and adds no coverage of its own. The library's every operation waits
// for its answer; here each returns at once, with the number of its call, and answers later — as a signal,
// and to the functions the document passed, if it passed any — on the thread the object lives on. What is
// read from the channel arrives the same way: the opening picture as properties, a subscription's
// deliveries and a stream's frames as signals.
//
// Nothing is sent that the document did not ask for: a confirmation is the document's, as it is the
// library's author's. What the contract carries is surfaced as plain objects of its fields (convert.hpp);
// a payload is bytes, never decoded.
#ifndef YOKE_QML_BINDING_HPP
#define YOKE_QML_BINDING_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include <QFileSystemWatcher>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QQmlParserStatus>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace yoke::iface {
class Attachment;
}

namespace yoke::qml {

// Hands work to an object on its own thread for as long as the object wants it; once shut, what is handed
// is dropped. Each opening of an attachment, a subscription or a delivery has its own, so what a thread
// left over from an earlier one finds is never applied to a later one.
class Gate {
  public:
    explicit Gate(QObject *target) : target_(target) {}

    // Whether f was handed over.
    template <typename F> bool post(F &&f) {
        std::lock_guard<std::mutex> hold(mu_);
        if (target_ == nullptr) {
            return false;
        }
        QMetaObject::invokeMethod(target_, std::forward<F>(f), Qt::QueuedConnection);
        return true;
    }

    void shut() {
        std::lock_guard<std::mutex> hold(mu_);
        target_ = nullptr;
    }

    bool open() {
        std::lock_guard<std::mutex> hold(mu_);
        return target_ != nullptr;
    }

  private:
    std::mutex mu_;
    QObject *target_;
};

// A client attached to a channel. It attaches when the document is complete and `active` holds, to the
// channel `address` names, or to `channel` of the application instance `instance`; changing either
// attaches again.
//
// Where the document sets `attachAgain`, an attachment that ended — the Core ended it, went away, or was
// not there — is attached again once its channel is back, which it learns from the host — its socket
// appearing — and never by trying at an interval; a channel reached at an address is not watched, and an
// attachment to one stays ended. Nothing about it is hidden: the status reads
// `Ended`, with its failure, until the next attachment is admitted; that one is given its own opening, and
// the picture is replaced with it; subscriptions and deliveries open again on it as new ones, claiming no
// continuity with the old; and a call in flight when the attachment ended is answered as refused, and
// never sent again.
class Attachment : public QObject, public QQmlParserStatus {
    Q_OBJECT
    QML_ELEMENT
    Q_INTERFACES(QQmlParserStatus)

    Q_PROPERTY(QString address READ address WRITE setAddress NOTIFY addressChanged)
    Q_PROPERTY(QString instance READ instance WRITE setInstance NOTIFY addressChanged)
    Q_PROPERTY(QString channel READ channel WRITE setChannel NOTIFY addressChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool attachAgain READ attachAgain WRITE setAttachAgain NOTIFY attachAgainChanged)

    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool attached READ attached NOTIFY statusChanged)
    Q_PROPERTY(QVariantMap failure READ failure NOTIFY statusChanged)
    Q_PROPERTY(int version READ version NOTIFY statusChanged)
    Q_PROPERTY(QString subscription READ subscription NOTIFY statusChanged)

    Q_PROPERTY(QVariantMap picture READ picture NOTIFY pictureChanged)
    Q_PROPERTY(QVariantList records READ records NOTIFY pictureChanged)

  public:
    // Detached: not asked to attach, or nowhere to. Attaching: waiting for the opening. Attached: the
    // opening arrived. Ended: the attachment failed or the Core ended it; `failure` says how.
    enum Status { Detached, Attaching, Attached, Ended };
    Q_ENUM(Status)

    explicit Attachment(QObject *parent = nullptr);
    ~Attachment() override;

    QString address() const { return address_; }
    void setAddress(const QString &address);
    QString instance() const { return instance_; }
    void setInstance(const QString &instance);
    QString channel() const { return channel_; }
    void setChannel(const QString &channel);
    bool active() const { return active_; }
    void setActive(bool active);
    bool attachAgain() const { return again_; }
    void setAttachAgain(bool again);

    Status status() const { return status_; }
    bool attached() const { return status_ == Attached; }
    QVariantMap failure() const { return failure_; }
    int version() const { return version_; }
    QString subscription() const { return subscription_; }
    QVariantMap picture() const { return picture_; }
    QVariantList records() const { return picture_.value("records").toList(); }

    // Each operation of the contract the document may issue: it returns the number of its call at once,
    // and answers later with `answered` or `refused`, and to `then` or `otherwise` where they are
    // functions.
    Q_INVOKABLE int read(const QString &kind, const QString &identity = {}, const QJSValue &then = {},
                         const QJSValue &otherwise = {});
    Q_INVOKABLE int command(const QString &unit, const QString &type, const QVariant &payload = {},
                            const QJSValue &then = {}, const QJSValue &otherwise = {});
    Q_INVOKABLE int ask(const QString &unit, const QString &type, const QVariant &payload = {},
                        const QJSValue &then = {}, const QJSValue &otherwise = {});
    Q_INVOKABLE int startStream(const QString &unit, const QString &stream, const QJSValue &then = {},
                                const QJSValue &otherwise = {});
    Q_INVOKABLE int stopStream(const QString &unit, const QString &stream, const QJSValue &then = {},
                               const QJSValue &otherwise = {});
    Q_INVOKABLE int confirm(const QString &subscription, double sequence, const QJSValue &then = {},
                            const QJSValue &otherwise = {});

    // A payload's bytes as text, read as UTF-8 — a convenience for a payload the document knows is text.
    Q_INVOKABLE QString text(const QVariant &bytes) const;

    void classBegin() override {}
    void componentComplete() override;

    // The library's attachment, while the status is Attached; nullptr otherwise.
    std::shared_ptr<yoke::iface::Attachment> native() const { return native_; }

  signals:
    void addressChanged();
    void activeChanged();
    void attachAgainChanged();
    void statusChanged();
    void pictureChanged();
    // An event of the standing subscription, in order.
    void received(const QVariantMap &event);
    void answered(int call, const QVariant &answer);
    void refused(int call, const QVariantMap &refusal);

  private:
    using Work = std::function<QVariant(yoke::iface::Attachment &)>;
    int issue(Work work, const QJSValue &then, const QJSValue &otherwise);
    int refuse(const QJSValue &then, const QJSValue &otherwise, const QVariantMap &failure);
    void reattach();
    void attach(std::chrono::milliseconds wait);
    void again();
    void detach();
    void end(const QVariantMap &failure);
    void settle(int call, bool ok, const QVariant &result);
    void settleAll(const QVariantMap &refusal);

    QString address_, instance_, channel_;
    bool active_ = true;
    bool complete_ = false;
    bool again_ = false;
    // The socket last tried, as the host identifies it — a socket made again is another — and what watches
    // the directory it is made in.
    QString tried_;
    QFileSystemWatcher *watch_ = nullptr;

    Status status_ = Detached;
    QVariantMap failure_;
    int version_ = 0;
    QString subscription_;
    QVariantMap picture_;

    int calls_ = 0;
    // The functions each call still unanswered was given, held on this object's thread.
    std::map<int, std::pair<QJSValue, QJSValue>> pending_;
    std::shared_ptr<Gate> gate_;
    std::shared_ptr<yoke::iface::Attachment> native_;
};

// A subscription of the attachment's channel, narrowed by a filter. It opens when the attachment is
// attached and `active` holds, and again whenever the filter changes; it delivers its snapshot, then its
// events and its overflows, in order.
class Subscription : public QObject, public QQmlParserStatus {
    Q_OBJECT
    QML_ELEMENT
    Q_INTERFACES(QQmlParserStatus)

    Q_PROPERTY(yoke::qml::Attachment *attachment READ attachment WRITE setAttachment NOTIFY attachmentChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QString subjectKind MEMBER subjectKind_ NOTIFY filterChanged)
    Q_PROPERTY(QString subjectIdentity MEMBER subjectIdentity_ NOTIFY filterChanged)
    Q_PROPERTY(int floor MEMBER floor_ NOTIFY filterChanged)
    Q_PROPERTY(QString type MEMBER type_ NOTIFY filterChanged)
    Q_PROPERTY(bool typePrefix MEMBER typePrefix_ NOTIFY filterChanged)
    Q_PROPERTY(QString occurrence MEMBER occurrence_ NOTIFY filterChanged)
    Q_PROPERTY(bool occurrencePrefix MEMBER occurrencePrefix_ NOTIFY filterChanged)

    Q_PROPERTY(bool open READ open NOTIFY openChanged)
    Q_PROPERTY(QString id READ id NOTIFY openChanged)
    Q_PROPERTY(QVariantMap failure READ failure NOTIFY openChanged)

  public:
    explicit Subscription(QObject *parent = nullptr);
    ~Subscription() override;

    Attachment *attachment() const { return attachment_; }
    void setAttachment(Attachment *attachment);
    bool active() const { return active_; }
    void setActive(bool active);
    bool open() const { return open_; }
    QString id() const { return id_; }
    QVariantMap failure() const { return failure_; }

    // States that the document has processed the subscription up to sequence; as the attachment's.
    Q_INVOKABLE int confirm(double sequence, const QJSValue &then = {}, const QJSValue &otherwise = {});

    void classBegin() override {}
    void componentComplete() override;

  signals:
    void attachmentChanged();
    void activeChanged();
    void filterChanged();
    void openChanged();
    void snapshot(const QVariantMap &snapshot);
    void received(const QVariantMap &event);
    void overflow(const QVariantMap &snapshot);
    // The subscription ended: completed by the Core, refused, or its attachment ended.
    void ended();

  private:
    struct Held;
    void reopen();
    void follow();
    void close();

    QPointer<Attachment> attachment_;
    bool active_ = true;
    bool complete_ = false;
    QString subjectKind_, subjectIdentity_, type_, occurrence_;
    int floor_ = 0;
    bool typePrefix_ = false, occurrencePrefix_ = false;

    bool open_ = false;
    QString id_;
    QVariantMap failure_;
    std::shared_ptr<Gate> gate_;
    std::shared_ptr<Held> held_;
    // The library's attachment it was opened on.
    std::weak_ptr<yoke::iface::Attachment> on_;
};

// A copy of a unit's stream, delivered to the attachment. It opens when the attachment is attached and
// `active` holds, and again whenever the unit or the stream changes; each frame arrives as `frame`, in
// order, until the delivery ends.
class Delivery : public QObject, public QQmlParserStatus {
    Q_OBJECT
    QML_ELEMENT
    Q_INTERFACES(QQmlParserStatus)

    Q_PROPERTY(yoke::qml::Attachment *attachment READ attachment WRITE setAttachment NOTIFY attachmentChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QString unit MEMBER unit_ NOTIFY streamChanged)
    Q_PROPERTY(QString stream MEMBER stream_ NOTIFY streamChanged)

    Q_PROPERTY(bool open READ open NOTIFY openChanged)
    Q_PROPERTY(QString id READ id NOTIFY openChanged)
    Q_PROPERTY(bool flowing READ flowing NOTIFY openChanged)
    Q_PROPERTY(QString arrives READ arrives NOTIFY openChanged)
    Q_PROPERTY(QVariantMap failure READ failure NOTIFY openChanged)

  public:
    explicit Delivery(QObject *parent = nullptr);
    ~Delivery() override;

    Attachment *attachment() const { return attachment_; }
    void setAttachment(Attachment *attachment);
    bool active() const { return active_; }
    void setActive(bool active);
    bool open() const { return open_; }
    QString id() const { return id_; }
    bool flowing() const { return flowing_; }
    QString arrives() const { return arrives_; }
    QVariantMap failure() const { return failure_; }

    void classBegin() override {}
    void componentComplete() override;

  signals:
    void attachmentChanged();
    void activeChanged();
    void streamChanged();
    void openChanged();
    // One frame: its `sequence`, the producer's clock `sentAt`, and its `payload`, as bytes.
    void frame(const QVariantMap &frame);
    void ended();

  private:
    struct Held;
    void reopen();
    void follow();
    void close();

    QPointer<Attachment> attachment_;
    bool active_ = true;
    bool complete_ = false;
    QString unit_, stream_;

    bool open_ = false;
    QString id_;
    bool flowing_ = false;
    QString arrives_;
    QVariantMap failure_;
    std::shared_ptr<Gate> gate_;
    std::shared_ptr<Held> held_;
    // The library's attachment it was opened on.
    std::weak_ptr<yoke::iface::Attachment> on_;
};

} // namespace yoke::qml

#endif
