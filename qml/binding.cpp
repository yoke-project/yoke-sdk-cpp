#include "binding.hpp"

#include <cstdlib>
#include <sys/stat.h>
#include <thread>

#include <QDebug>
#include <QFileInfo>
#include <QJSEngine>
#include <QQmlEngine>

#include "convert.hpp"
#include "yoke/interface.hpp"

namespace yoke::qml {

namespace yi = yoke::iface;

namespace {

QString q(const std::string &s) { return QString::fromStdString(s); }

std::string s(const QString &q) { return q.toStdString(); }

QVariantMap failed(const char *code, const QString &message) {
    return {{"code", QString::fromLatin1(code)}, {"message", message}};
}

QVariantMap unpayable() {
    return failed("binding.payload", QStringLiteral("a payload is a string or bytes, and this is neither"));
}

// Runs f on a thread of its own, which nobody waits for: what it finds it hands through a gate.
template <typename F> void apart(F &&f) { std::thread(std::forward<F>(f)).detach(); }

// Lets an attachment of the library go, off the object's thread: closing waits for the Core.
void let_go(std::shared_ptr<yi::Attachment> a) {
    if (a) {
        apart([a = std::move(a)] {
            try {
                a->close();
            } catch (...) {
            }
        });
    }
}

// Calls a function the document passed, with one value, and says where it threw.
void call_back(QObject *owner, const QJSValue &f, const QVariant &value) {
    if (!f.isCallable()) {
        return;
    }
    QJSEngine *engine = qjsEngine(owner);
    if (engine == nullptr) {
        return;
    }
    QJSValue result = QJSValue(f).call({engine->toScriptValue(value)});
    if (result.isError()) {
        qWarning().noquote() << "yoke: a function given to a call threw:" << result.toString();
    }
}

} // namespace

// ---- Attachment ----------------------------------------------------------------------------------------

Attachment::Attachment(QObject *parent) : QObject(parent) {}

Attachment::~Attachment() { detach(); }

void Attachment::setAddress(const QString &address) {
    if (address_ == address) {
        return;
    }
    address_ = address;
    emit addressChanged();
    reattach();
}

void Attachment::setInstance(const QString &instance) {
    if (instance_ == instance) {
        return;
    }
    instance_ = instance;
    emit addressChanged();
    reattach();
}

void Attachment::setChannel(const QString &channel) {
    if (channel_ == channel) {
        return;
    }
    channel_ = channel;
    emit addressChanged();
    reattach();
}

void Attachment::setActive(bool active) {
    if (active_ == active) {
        return;
    }
    active_ = active;
    emit activeChanged();
    reattach();
}

void Attachment::setAttachAgain(bool again) {
    if (again_ == again) {
        return;
    }
    again_ = again;
    emit attachAgainChanged();
    if (!again_ && watch_ != nullptr && !watch_->directories().isEmpty()) {
        watch_->removePaths(watch_->directories());
    }
    this->again();
}

void Attachment::componentComplete() {
    complete_ = true;
    reattach();
}

namespace {

// How long a try waits for the opening, once the channel's socket is there.
constexpr std::chrono::milliseconds try_within{2000};

// A socket as the host identifies it, or empty where there is none: made again, it is another.
QString identity(const QString &path) {
    struct stat st {};
    if (::stat(path.toLocal8Bit().constData(), &st) != 0 || !S_ISSOCK(st.st_mode)) {
        return {};
    }
    return QStringLiteral("%1:%2.%3").arg(st.st_ino).arg(st.st_ctim.tv_sec).arg(st.st_ctim.tv_nsec);
}

// The nearest directory of a path that exists: where the host says a socket was made.
QString nearest(const QString &path) {
    QString dir = QFileInfo(path).absolutePath();
    while (!QFileInfo::exists(dir) && dir != QStringLiteral("/")) {
        dir = QFileInfo(dir).absolutePath();
    }
    return dir;
}

} // namespace

void Attachment::again() {
    if (!again_ || !active_ || !complete_ || status_ != Ended) {
        return;
    }
    QString where = address_;
    if (where.isEmpty() && !instance_.isEmpty() && !channel_.isEmpty()) {
        try {
            where =
                q(yi::application(s(instance_), s(channel_), [](const char *n) { return std::getenv(n); }));
        } catch (...) {
            return;
        }
    }
    // An address is not watched: there is nothing on this host to say it is back.
    if (!where.startsWith('/')) {
        return;
    }
    // A socket not tried yet is a channel that may be back; the one that was tried is waited past.
    QString now = identity(where);
    if (!now.isEmpty() && now != tried_) {
        tried_ = now;
        attach(try_within);
        return;
    }
    if (watch_ == nullptr) {
        watch_ = new QFileSystemWatcher(this);
        connect(watch_, &QFileSystemWatcher::directoryChanged, this, [this] { again(); });
    }
    if (!watch_->directories().isEmpty()) {
        watch_->removePaths(watch_->directories());
    }
    watch_->addPath(nearest(where));
    // A socket made while the watch was being set is one the watch did not see.
    QString since = identity(where);
    if (!since.isEmpty() && since != tried_) {
        tried_ = since;
        attach(try_within);
    }
}

void Attachment::detach() {
    if (gate_) {
        gate_->shut();
        gate_.reset();
    }
    let_go(std::move(native_));
    native_.reset();
}

void Attachment::end(const QVariantMap &failure) {
    detach();
    status_ = Ended;
    failure_ = failure;
    version_ = 0;
    subscription_.clear();
    settleAll(failure);
    emit statusChanged();
    again();
}

void Attachment::reattach() {
    if (!complete_) {
        return;
    }
    if (watch_ != nullptr && !watch_->directories().isEmpty()) {
        watch_->removePaths(watch_->directories());
    }
    attach(yi::answer_within);
}

void Attachment::attach(std::chrono::milliseconds wait) {
    bool was = status_ != Detached;
    detach();
    settleAll(failed("binding.detached", QStringLiteral("the attachment was let go before it answered")));

    QString where = address_;
    if (where.isEmpty() && !instance_.isEmpty() && !channel_.isEmpty()) {
        try {
            where =
                q(yi::application(s(instance_), s(channel_), [](const char *n) { return std::getenv(n); }));
        } catch (...) {
            end(refusal(std::current_exception()));
            return;
        }
    }
    if (!active_ || where.isEmpty()) {
        status_ = Detached;
        failure_.clear();
        version_ = 0;
        subscription_.clear();
        if (was) {
            emit statusChanged();
        }
        return;
    }

    if (where.startsWith('/')) {
        tried_ = identity(where);
    }
    status_ = Attaching;
    failure_.clear();
    version_ = 0;
    subscription_.clear();
    emit statusChanged();

    auto gate = std::make_shared<Gate>(this);
    gate_ = gate;
    apart([this, gate, where = s(where), wait] {
        try {
            std::shared_ptr<yi::Attachment> a(yi::Attachment::attach(where, {}, wait));
            QVariantMap picture = variant(a->picture());
            int version = a->version();
            yi::Subscription *standing = a->standing();
            QString id = standing != nullptr ? q(standing->id()) : QString();
            bool handed = gate->post([this, gate, a, picture, version, id] {
                if (gate_ != gate) {
                    let_go(a);
                    return;
                }
                native_ = a;
                if (watch_ != nullptr && !watch_->directories().isEmpty()) {
                    watch_->removePaths(watch_->directories());
                }
                status_ = Attached;
                version_ = version;
                subscription_ = id;
                picture_ = picture;
                emit pictureChanged();
                emit statusChanged();
            });
            if (!handed) {
                let_go(a);
                return;
            }
            if (standing == nullptr) {
                return;
            }
            // The standing subscription, read here until the attachment ends: its events as they come, an
            // overflow's snapshot as the picture.
            for (;;) {
                auto d = standing->next();
                if (!d) {
                    gate->post([this, gate] {
                        if (gate_ == gate) {
                            end(failed("attachment.ended", QStringLiteral("the Core completed the standing "
                                                                          "subscription")));
                        }
                    });
                    return;
                }
                if (d->has_event()) {
                    QVariantMap e = variant(d->event());
                    gate->post([this, e] { emit received(e); });
                } else if (d->has_overflow() || d->has_snapshot()) {
                    QVariantMap p = variant(d->has_overflow() ? d->overflow() : d->snapshot());
                    gate->post([this, p] {
                        picture_ = p;
                        emit pictureChanged();
                    });
                }
            }
        } catch (...) {
            QVariantMap f = refusal(std::current_exception());
            gate->post([this, gate, f] {
                if (gate_ == gate) {
                    end(f);
                }
            });
        }
    });
}

int Attachment::issue(Work work, const QJSValue &then, const QJSValue &otherwise) {
    auto a = native_;
    auto gate = gate_;
    if (!a || !gate) {
        return refuse(then, otherwise,
                      failed("binding.detached", QStringLiteral("the attachment is not attached")));
    }
    int call = ++calls_;
    pending_.emplace(call, std::make_pair(then, otherwise));
    apart([this, call, a, gate, work = std::move(work)] {
        QVariant result;
        bool ok = true;
        try {
            result = work(*a);
        } catch (...) {
            ok = false;
            result = refusal(std::current_exception());
        }
        gate->post([this, call, ok, result] { settle(call, ok, result); });
    });
    return call;
}

int Attachment::refuse(const QJSValue &then, const QJSValue &otherwise, const QVariantMap &failure) {
    int call = ++calls_;
    pending_.emplace(call, std::make_pair(then, otherwise));
    // Answered later all the same, so a document reads every call one way.
    QMetaObject::invokeMethod(
        this, [this, call, failure] { settle(call, false, failure); }, Qt::QueuedConnection);
    return call;
}

void Attachment::settle(int call, bool ok, const QVariant &result) {
    auto it = pending_.find(call);
    if (it == pending_.end()) {
        return;
    }
    auto [then, otherwise] = it->second;
    pending_.erase(it);
    if (ok) {
        emit answered(call, result);
        call_back(this, then, result);
    } else {
        QVariantMap f = result.toMap();
        emit refused(call, f);
        call_back(this, otherwise, f);
    }
}

void Attachment::settleAll(const QVariantMap &refusal) {
    auto calls = std::move(pending_);
    pending_.clear();
    for (auto &[call, fs] : calls) {
        emit refused(call, refusal);
        call_back(this, fs.second, refusal);
    }
}

int Attachment::read(const QString &kind, const QString &identity, const QJSValue &then,
                     const QJSValue &otherwise) {
    return issue(
        [kind = s(kind), identity = s(identity)](yi::Attachment &a) -> QVariant {
            QVariantList out;
            for (const auto &r : a.read(kind, identity)) {
                out.append(variant(r));
            }
            return out;
        },
        then, otherwise);
}

int Attachment::command(const QString &unit, const QString &type, const QVariant &payload,
                        const QJSValue &then, const QJSValue &otherwise) {
    QByteArray bytes;
    if (!yoke::qml::payload(payload, bytes)) {
        return refuse(then, otherwise, unpayable());
    }
    return issue([unit = s(unit), type = s(type), bytes = bytes.toStdString()](
                     yi::Attachment &a) -> QVariant { return variant(a.command(unit, type, bytes)); },
                 then, otherwise);
}

int Attachment::ask(const QString &unit, const QString &type, const QVariant &payload, const QJSValue &then,
                    const QJSValue &otherwise) {
    QByteArray bytes;
    if (!yoke::qml::payload(payload, bytes)) {
        return refuse(then, otherwise, unpayable());
    }
    return issue(
        [unit = s(unit), type = s(type), bytes = bytes.toStdString()](yi::Attachment &a) -> QVariant {
            std::string answer = a.ask(unit, type, bytes);
            return QByteArray(answer.data(), static_cast<qsizetype>(answer.size()));
        },
        then, otherwise);
}

int Attachment::startStream(const QString &unit, const QString &stream, const QJSValue &then,
                            const QJSValue &otherwise) {
    return issue([unit = s(unit), stream = s(stream)](
                     yi::Attachment &a) -> QVariant { return variant(a.start_stream(unit, stream)); },
                 then, otherwise);
}

int Attachment::stopStream(const QString &unit, const QString &stream, const QJSValue &then,
                           const QJSValue &otherwise) {
    return issue([unit = s(unit), stream = s(stream)](
                     yi::Attachment &a) -> QVariant { return variant(a.stop_stream(unit, stream)); },
                 then, otherwise);
}

int Attachment::confirm(const QString &subscription, double sequence, const QJSValue &then,
                        const QJSValue &otherwise) {
    return issue(
        [subscription = s(subscription),
         sequence = static_cast<std::uint64_t>(sequence)](yi::Attachment &a) -> QVariant {
            a.confirm(subscription, sequence);
            return true;
        },
        then, otherwise);
}

QString Attachment::text(const QVariant &bytes) const {
    if (bytes.typeId() == QMetaType::QByteArray) {
        return QString::fromUtf8(bytes.toByteArray());
    }
    return bytes.toString();
}

// ---- Subscription --------------------------------------------------------------------------------------

// The library's subscription, once it is open, and whether it was let go before it was.
struct Subscription::Held {
    std::mutex mu;
    bool closed = false;
    std::shared_ptr<yi::Subscription> sub;

    // Keeps sub, unless it was let go meanwhile.
    bool keep(std::shared_ptr<yi::Subscription> s) {
        std::lock_guard<std::mutex> hold(mu);
        if (closed) {
            return false;
        }
        sub = std::move(s);
        return true;
    }

    void let_go() {
        std::shared_ptr<yi::Subscription> s;
        {
            std::lock_guard<std::mutex> hold(mu);
            closed = true;
            s = sub;
        }
        if (s) {
            apart([s] {
                try {
                    s->close();
                } catch (...) {
                }
            });
        }
    }
};

Subscription::Subscription(QObject *parent) : QObject(parent) {
    connect(this, &Subscription::filterChanged, this, &Subscription::reopen);
}

Subscription::~Subscription() {
    if (gate_) {
        gate_->shut();
    }
    if (held_) {
        held_->let_go();
    }
}

void Subscription::setAttachment(Attachment *attachment) {
    if (attachment_ == attachment) {
        return;
    }
    if (attachment_) {
        disconnect(attachment_, nullptr, this, nullptr);
    }
    attachment_ = attachment;
    if (attachment_) {
        connect(attachment_, &Attachment::statusChanged, this, &Subscription::follow);
    }
    emit attachmentChanged();
    reopen();
}

void Subscription::setActive(bool active) {
    if (active_ == active) {
        return;
    }
    active_ = active;
    emit activeChanged();
    reopen();
}

void Subscription::follow() {
    // Still open on the attachment the library holds: nothing changed for this one.
    auto a = attachment_ ? attachment_->native() : nullptr;
    if (gate_ && a && a == on_.lock()) {
        return;
    }
    reopen();
}

void Subscription::componentComplete() {
    complete_ = true;
    reopen();
}

void Subscription::close() {
    if (gate_) {
        gate_->shut();
        gate_.reset();
    }
    if (held_) {
        held_->let_go();
        held_.reset();
    }
    if (open_) {
        open_ = false;
        id_.clear();
        emit openChanged();
        emit ended();
    }
}

void Subscription::reopen() {
    if (!complete_) {
        return;
    }
    auto a = attachment_ ? attachment_->native() : nullptr;
    close();
    if (!active_ || !a) {
        return;
    }
    yi::v1::Filter filter;
    filter.set_subject_kind(s(subjectKind_));
    filter.set_subject_identity(s(subjectIdentity_));
    filter.set_floor(static_cast<std::uint32_t>(floor_));
    filter.set_type(s(type_));
    filter.set_type_prefix(typePrefix_);
    filter.set_occurrence(s(occurrence_));
    filter.set_occurrence_prefix(occurrencePrefix_);

    auto gate = std::make_shared<Gate>(this);
    auto held = std::make_shared<Held>();
    gate_ = gate;
    held_ = held;
    on_ = a;
    failure_.clear();
    apart([this, a, filter, gate, held] {
        auto finish = [this, gate](QVariantMap f) {
            gate->post([this, gate, f] {
                if (gate_ != gate) {
                    return;
                }
                failure_ = f;
                gate_->shut();
                gate_.reset();
                held_.reset();
                bool was = open_;
                open_ = false;
                id_.clear();
                emit openChanged();
                if (was || !f.isEmpty()) {
                    emit ended();
                }
            });
        };
        try {
            std::shared_ptr<yi::Subscription> sub(a->subscribe(filter));
            if (!held->keep(sub)) {
                sub->close();
                return;
            }
            QString id = q(sub->id());
            for (;;) {
                auto d = sub->next();
                if (!d) {
                    break;
                }
                if (d->has_snapshot()) {
                    QVariantMap m = variant(d->snapshot());
                    gate->post([this, gate, id, m] {
                        if (gate_ != gate) {
                            return;
                        }
                        open_ = true;
                        id_ = id;
                        emit openChanged();
                        emit snapshot(m);
                    });
                } else if (d->has_event()) {
                    QVariantMap e = variant(d->event());
                    gate->post([this, e] { emit received(e); });
                } else if (d->has_overflow()) {
                    QVariantMap m = variant(d->overflow());
                    gate->post([this, m] { emit overflow(m); });
                }
            }
            finish({});
        } catch (...) {
            finish(refusal(std::current_exception()));
        }
    });
}

int Subscription::confirm(double sequence, const QJSValue &then, const QJSValue &otherwise) {
    if (!attachment_) {
        return 0;
    }
    return attachment_->confirm(id_, sequence, then, otherwise);
}

// ---- Delivery ------------------------------------------------------------------------------------------

struct Delivery::Held {
    std::mutex mu;
    bool closed = false;
    std::shared_ptr<yi::Delivery> delivery;

    bool keep(std::shared_ptr<yi::Delivery> d) {
        std::lock_guard<std::mutex> hold(mu);
        if (closed) {
            return false;
        }
        delivery = std::move(d);
        return true;
    }

    void let_go() {
        std::shared_ptr<yi::Delivery> d;
        {
            std::lock_guard<std::mutex> hold(mu);
            closed = true;
            d = delivery;
        }
        if (d) {
            // Releasing waits for the Core's answer.
            apart([d] {
                try {
                    d->release();
                } catch (...) {
                }
            });
        }
    }
};

Delivery::Delivery(QObject *parent) : QObject(parent) {
    connect(this, &Delivery::streamChanged, this, &Delivery::reopen);
}

Delivery::~Delivery() {
    if (gate_) {
        gate_->shut();
    }
    if (held_) {
        held_->let_go();
    }
}

void Delivery::setAttachment(Attachment *attachment) {
    if (attachment_ == attachment) {
        return;
    }
    if (attachment_) {
        disconnect(attachment_, nullptr, this, nullptr);
    }
    attachment_ = attachment;
    if (attachment_) {
        connect(attachment_, &Attachment::statusChanged, this, &Delivery::follow);
    }
    emit attachmentChanged();
    reopen();
}

void Delivery::setActive(bool active) {
    if (active_ == active) {
        return;
    }
    active_ = active;
    emit activeChanged();
    reopen();
}

void Delivery::follow() {
    // Still open on the attachment the library holds: nothing changed for this one.
    auto a = attachment_ ? attachment_->native() : nullptr;
    if (gate_ && a && a == on_.lock()) {
        return;
    }
    reopen();
}

void Delivery::componentComplete() {
    complete_ = true;
    reopen();
}

void Delivery::close() {
    if (gate_) {
        gate_->shut();
        gate_.reset();
    }
    if (held_) {
        held_->let_go();
        held_.reset();
    }
    if (open_) {
        open_ = false;
        id_.clear();
        flowing_ = false;
        arrives_.clear();
        emit openChanged();
        emit ended();
    }
}

void Delivery::reopen() {
    if (!complete_) {
        return;
    }
    auto a = attachment_ ? attachment_->native() : nullptr;
    close();
    if (!active_ || !a || unit_.isEmpty() || stream_.isEmpty()) {
        return;
    }
    auto gate = std::make_shared<Gate>(this);
    auto held = std::make_shared<Held>();
    gate_ = gate;
    held_ = held;
    on_ = a;
    failure_.clear();
    apart([this, a, gate, held, unit = s(unit_), stream = s(stream_)] {
        auto finish = [this, gate](QVariantMap f) {
            gate->post([this, gate, f] {
                if (gate_ != gate) {
                    return;
                }
                failure_ = f;
                gate_->shut();
                gate_.reset();
                held_.reset();
                bool was = open_;
                open_ = false;
                id_.clear();
                flowing_ = false;
                arrives_.clear();
                emit openChanged();
                if (was || !f.isEmpty()) {
                    emit ended();
                }
            });
        };
        try {
            std::shared_ptr<yi::Delivery> d(a->subscribe_stream(unit, stream));
            if (!held->keep(d)) {
                try {
                    d->release();
                } catch (...) {
                }
                return;
            }
            QString id = q(d->id());
            bool flowing = d->flowing();
            QString arrives = q(d->arrives());
            gate->post([this, gate, id, flowing, arrives] {
                if (gate_ != gate) {
                    return;
                }
                open_ = true;
                id_ = id;
                flowing_ = flowing;
                arrives_ = arrives;
                emit openChanged();
            });
            for (;;) {
                auto f = d->next();
                if (!f) {
                    break;
                }
                QVariantMap m{
                    {"sequence", QVariant::fromValue<quint64>(f->sequence)},
                    {"sentAt", QVariant::fromValue<quint64>(f->sent_at)},
                    {"payload", QByteArray(f->payload.data(), static_cast<qsizetype>(f->payload.size()))}};
                gate->post([this, m] { emit frame(m); });
            }
            finish({});
        } catch (...) {
            finish(refusal(std::current_exception()));
        }
    });
}

} // namespace yoke::qml
