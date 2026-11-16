// What the contract carries, as a QML document reads it: a message as an object of its fields, named as
// the contract's JSON mapping names them, and a refusal as an object of its code and its detail.
#ifndef YOKE_QML_CONVERT_HPP
#define YOKE_QML_CONVERT_HPP

#include <exception>

#include <QByteArray>
#include <QVariant>
#include <QVariantMap>

#include <google/protobuf/message.h>

namespace yoke::qml {

// A message as an object: each field under its lowerCamel name; a scalar always, at its default where it
// was not set; a message only where it was set, and of a oneof only the member set; a repeated field as a
// list; bytes as bytes, never decoded; an enumeration by its value's name; a Timestamp as a date.
QVariantMap variant(const google::protobuf::Message &message);

// What a call failed with, as an object: `code`, `message`, and where the refusal names them `stage`,
// `item` and `subject`; a suspension's `grade` and `by`. A failure that is no refusal carries the code
// `binding.failed`, and an attachment that ended `attachment.ended`.
QVariantMap refusal(const std::exception_ptr &failure);

// A payload as the operations take it: a string as its UTF-8, bytes as they are, and nothing else.
bool payload(const QVariant &value, QByteArray &out);

} // namespace yoke::qml

#endif
