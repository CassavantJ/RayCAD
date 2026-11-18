// The command palette's argument form (task T-021), generated from an action's JSON Schema
// (api::ActionInfo::argsSchemaJson). No QObject here.
//
// Supported: the top-level properties of an object schema. Strings, numbers, integers, booleans and enums get their
// own control; anything else (arrays, objects) is typed as JSON text. Properties whose name says they hold a file
// ("path", "file", "...Path", "...File") get a browse button. The "document" property is never shown: actions act on
// the active document without it.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>

namespace ray::ui::palette {

// One QVariantMap per field, required fields first (in the schema's "required" order), then the optional ones by
// name. Keys:
//   name, label, description   the property, a readable label ("chordTolerance" -> "Chord tolerance") and its text
//   kind                       "string", "number", "integer", "boolean", "enum" or "json"
//   required, hasDefault       bools; defaultText / defaultBool hold the default when there is one
//   choices                    enum values as text (kind "enum")
//   path, save, filters        a file path; whether the browse dialog saves (save-as, export); dialog name filters
QVariantList formFields(const QString& actionId, const QString& schemaJson);

// True when the action cannot run without arguments: a required property other than "document", or an
// anyOf / oneOf rule that an empty object cannot meet.
bool needsForm(const QString& schemaJson);

// The schema declares this top-level property.
bool hasProperty(const QString& schemaJson, const QString& name);

// The arguments object from the form's values (field name -> text, or bool for booleans). Empty optional fields are
// left out, so the action's defaults apply. ok false: `error` says what is wrong and `field` names the field.
struct Args {
    bool ok = false;
    QString json; // compact JSON object
    QString error;
    QString field;
};
Args buildArgs(const QString& schemaJson, const QVariantMap& values);

} // namespace ray::ui::palette
