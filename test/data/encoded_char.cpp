/**
 * Test derived from SWISH++.
 *
 * Copyright (C) 1998-2026 Paul J. Lucas
 *
 * @sa https://github.com/paul-j-lucas/swishxx
 */

struct encoded_char_range {
  class decoder;
};

struct encoded_char_range::decoder {
  decoder() { }
  virtual ~decoder();
};

encoded_char_range::decoder::~decoder() {
}

struct base64_decoder : encoded_char_range::decoder {
};

int main() {
  static base64_decoder b64;
}
