"""The client-side validator follows the server's (src/command/src/Schema.cpp)."""

import unittest

from wabcut._schema import validate


def messages(schema, value):
    return [(i.path, i.message) for i in validate(schema, value)]


class SchemaTests(unittest.TestCase):
    def test_types(self):
        self.assertEqual(messages({"type": "string"}, 5), [("/", "expected string, got integer")])
        self.assertEqual(messages({"type": "integer"}, 2.0), [])  # a float with no fraction is an integer
        self.assertEqual(messages({"type": "integer"}, 2.5), [("/", "expected integer, got number")])
        self.assertEqual(messages({"type": "number"}, 3), [])
        self.assertEqual(messages({"type": ["integer", "string"]}, "00:00:01:00"), [])
        self.assertEqual(messages({"type": ["integer", "string"]}, None), [("/", "expected integer or string, got null")])

    def test_a_boolean_is_neither_an_integer_nor_a_number(self):
        self.assertEqual(messages({"type": "integer"}, True), [("/", "expected integer, got boolean")])
        self.assertEqual(messages({"type": "number"}, False), [("/", "expected number, got boolean")])
        self.assertEqual(messages({"type": "boolean"}, True), [])
        self.assertEqual(messages({"enum": [1]}, True), [("/", "must be one of [1]")])

    def test_objects_are_closed_by_additional_properties_false(self):
        schema = {"type": "object", "properties": {"a": {"type": "integer"}}, "required": ["a"], "additionalProperties": False}
        self.assertEqual(messages(schema, {"a": 1}), [])
        self.assertEqual(messages(schema, {}), [("/", 'is missing required "a"')])
        self.assertEqual(messages(schema, {"a": 1, "b": 2}), [("/b", "is not a known parameter")])
        self.assertEqual(messages(schema, {"a": "x"}), [("/a", "expected integer, got string")])
        self.assertEqual(messages({"type": "object", "properties": {}}, {"free": 1}), [])  # open objects allow extras

    def test_paths_are_json_pointers(self):
        schema = {"type": "object", "properties": {"clips": {"type": "array", "items": {"type": "object", "properties": {"a/b": {"type": "integer"}}}}}}
        self.assertEqual(messages(schema, {"clips": [{}, {"a/b": "x"}]}), [("/clips/1/a~1b", "expected integer, got string")])

    def test_bounds_and_sizes(self):
        self.assertEqual(messages({"type": "integer", "minimum": 1}, 0), [("/", "must be at least 1")])
        self.assertEqual(messages({"type": "number", "maximum": 1.5}, 2), [("/", "must be at most 1.5")])
        self.assertEqual(messages({"type": "number", "exclusiveMinimum": 0}, 0), [("/", "must be greater than 0")])
        self.assertEqual(messages({"type": "array", "minItems": 2}, [1]), [("/", "needs at least 2 items")])
        self.assertEqual(messages({"type": "array", "maxItems": 1}, [1, 2]), [("/", "allows at most 1 items")])
        self.assertEqual(messages({"type": "string", "maxLength": 3}, "abcd"), [("/", "must be at most 3 characters long")])

    def test_string_length_counts_utf8_bytes_like_the_server(self):
        self.assertEqual(messages({"type": "string", "maxLength": 3}, "éé"), [("/", "must be at most 3 characters long")])
        self.assertEqual(messages({"type": "string", "minLength": 2}, "é"), [])

    def test_enum_and_const(self):
        self.assertEqual(messages({"enum": ["a", "b"]}, "c"), [("/", 'must be one of ["a","b"]')])
        self.assertEqual(messages({"const": 3}, 4), [("/", "must be 3")])
        self.assertEqual(messages({"enum": [1.0]}, 1), [])

    def test_keywords_apply_only_to_their_own_type(self):
        schema = {"type": ["integer", "object"], "required": ["track"], "minimum": 1}
        self.assertEqual(messages(schema, 5), [])
        self.assertEqual(messages(schema, {"track": "V1"}), [])
        self.assertEqual(messages(schema, {}), [("/", 'is missing required "track"')])


if __name__ == "__main__":
    unittest.main()
