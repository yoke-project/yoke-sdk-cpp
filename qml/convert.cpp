#include "convert.hpp"

#include <QDateTime>
#include <QVariantList>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/timestamp.pb.h>

#include "yoke/base.hpp"
#include "yoke/interface.hpp"

namespace yoke::qml {

namespace {

// An enum's value by its name, or by its number where the definitions name none — a value a newer peer
// sent; read through the value, as every protobuf the family builds against offers it.
QVariant enumName(const google::protobuf::FieldDescriptor *f, int value) {
    const google::protobuf::EnumValueDescriptor *named = f->enum_type()->FindValueByNumber(value);
    return named ? QString::fromStdString(std::string(named->name())) : QString::number(value);
}

using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
using google::protobuf::Reflection;

bool timestamp(const google::protobuf::Descriptor *d) {
    return d->full_name() == "google.protobuf.Timestamp";
}

QVariant date(const Message &m) {
    const auto *r = m.GetReflection();
    const auto *d = m.GetDescriptor();
    qint64 seconds = r->GetInt64(m, d->FindFieldByName("seconds"));
    qint64 nanos = r->GetInt32(m, d->FindFieldByName("nanos"));
    return QDateTime::fromMSecsSinceEpoch(seconds * 1000 + nanos / 1000000).toUTC();
}

QVariant one(const Message &m, const FieldDescriptor *f) {
    const Reflection *r = m.GetReflection();
    switch (f->cpp_type()) {
    case FieldDescriptor::CPPTYPE_INT32:
        return r->GetInt32(m, f);
    case FieldDescriptor::CPPTYPE_INT64:
        return QVariant::fromValue<qint64>(r->GetInt64(m, f));
    case FieldDescriptor::CPPTYPE_UINT32:
        return r->GetUInt32(m, f);
    case FieldDescriptor::CPPTYPE_UINT64:
        return QVariant::fromValue<quint64>(r->GetUInt64(m, f));
    case FieldDescriptor::CPPTYPE_DOUBLE:
        return r->GetDouble(m, f);
    case FieldDescriptor::CPPTYPE_FLOAT:
        return r->GetFloat(m, f);
    case FieldDescriptor::CPPTYPE_BOOL:
        return r->GetBool(m, f);
    case FieldDescriptor::CPPTYPE_ENUM:
        return enumName(f, r->GetEnumValue(m, f));
    case FieldDescriptor::CPPTYPE_STRING: {
        std::string s = r->GetString(m, f);
        if (f->type() == FieldDescriptor::TYPE_BYTES) {
            return QByteArray(s.data(), static_cast<qsizetype>(s.size()));
        }
        return QString::fromStdString(s);
    }
    case FieldDescriptor::CPPTYPE_MESSAGE: {
        const Message &sub = r->GetMessage(m, f);
        return timestamp(f->message_type()) ? date(sub) : QVariant(variant(sub));
    }
    }
    return {};
}

QVariant repeated(const Message &m, const FieldDescriptor *f, int i) {
    const Reflection *r = m.GetReflection();
    switch (f->cpp_type()) {
    case FieldDescriptor::CPPTYPE_INT32:
        return r->GetRepeatedInt32(m, f, i);
    case FieldDescriptor::CPPTYPE_INT64:
        return QVariant::fromValue<qint64>(r->GetRepeatedInt64(m, f, i));
    case FieldDescriptor::CPPTYPE_UINT32:
        return r->GetRepeatedUInt32(m, f, i);
    case FieldDescriptor::CPPTYPE_UINT64:
        return QVariant::fromValue<quint64>(r->GetRepeatedUInt64(m, f, i));
    case FieldDescriptor::CPPTYPE_DOUBLE:
        return r->GetRepeatedDouble(m, f, i);
    case FieldDescriptor::CPPTYPE_FLOAT:
        return r->GetRepeatedFloat(m, f, i);
    case FieldDescriptor::CPPTYPE_BOOL:
        return r->GetRepeatedBool(m, f, i);
    case FieldDescriptor::CPPTYPE_ENUM:
        return enumName(f, r->GetRepeatedEnumValue(m, f, i));
    case FieldDescriptor::CPPTYPE_STRING: {
        std::string s = r->GetRepeatedString(m, f, i);
        if (f->type() == FieldDescriptor::TYPE_BYTES) {
            return QByteArray(s.data(), static_cast<qsizetype>(s.size()));
        }
        return QString::fromStdString(s);
    }
    case FieldDescriptor::CPPTYPE_MESSAGE: {
        const Message &sub = r->GetRepeatedMessage(m, f, i);
        return timestamp(f->message_type()) ? date(sub) : QVariant(variant(sub));
    }
    }
    return {};
}

} // namespace

QVariantMap variant(const Message &m) {
    QVariantMap out;
    const auto *d = m.GetDescriptor();
    const Reflection *r = m.GetReflection();
    for (int i = 0; i < d->field_count(); i++) {
        const FieldDescriptor *f = d->field(i);
        QString name = QString::fromStdString(std::string(f->json_name()));
        if (f->is_repeated()) {
            QVariantList list;
            for (int j = 0; j < r->FieldSize(m, f); j++) {
                list.append(repeated(m, f, j));
            }
            out.insert(name, list);
            continue;
        }
        // A message, or a member of a oneof, is there only where it was set.
        if ((f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE || f->containing_oneof() != nullptr) &&
            !r->HasField(m, f)) {
            continue;
        }
        out.insert(name, one(m, f));
    }
    return out;
}

QVariantMap refusal(const std::exception_ptr &failure) {
    QVariantMap out;
    try {
        std::rethrow_exception(failure);
    } catch (const yoke::iface::Suspended &s) {
        out["code"] = QString::fromStdString(s.code());
        out["message"] = QString::fromUtf8(s.what());
        out["grade"] = QString::fromStdString(s.grade());
        out["by"] = QString::fromStdString(s.by());
    } catch (const yoke::Refusal &r) {
        out["code"] = QString::fromStdString(r.code());
        out["message"] = QString::fromUtf8(r.what());
        if (!r.stage().empty()) {
            out["stage"] = QString::fromStdString(r.stage());
        }
        if (!r.item().empty()) {
            out["item"] = QString::fromStdString(r.item());
        }
        if (!r.subject().kind.empty() || !r.subject().identity.empty()) {
            out["subject"] =
                QVariantMap{{"kind", QString::fromStdString(r.subject().kind)},
                            {"identity", QString::fromStdString(r.subject().identity)},
                            {"incarnation", QVariant::fromValue<quint64>(r.subject().incarnation)}};
        }
    } catch (const yoke::iface::Ended &e) {
        out["code"] = QStringLiteral("attachment.ended");
        out["message"] = QString::fromUtf8(e.what());
    } catch (const std::exception &e) {
        out["code"] = QStringLiteral("binding.failed");
        out["message"] = QString::fromUtf8(e.what());
    } catch (...) {
        out["code"] = QStringLiteral("binding.failed");
        out["message"] = QStringLiteral("a failure that said nothing of itself");
    }
    return out;
}

bool payload(const QVariant &value, QByteArray &out) {
    if (!value.isValid() || value.isNull()) {
        out.clear();
        return true;
    }
    if (value.typeId() == QMetaType::QByteArray) {
        out = value.toByteArray();
        return true;
    }
    if (value.typeId() == QMetaType::QString) {
        out = value.toString().toUtf8();
        return true;
    }
    return false;
}

} // namespace yoke::qml
