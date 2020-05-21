namespace stage2 {

struct streaming_structural_parser: structural_parser {
  really_inline streaming_structural_parser(size_t len, parser &_doc_parser)
    : structural_parser(len, _doc_parser) {}

  // override to add streaming
  WARN_UNUSED really_inline error_code start(UNUSED size_t len) {
    log_start();
    init(); // sets is_valid to false
    // Capacity ain't no thang for streaming, so we don't check it.
    // Advance to the first character as soon as possible
    advance_char();
    // Push the root scope (there is always at least one scope)
    if (start_document()) {
      return on_error(DEPTH_ERROR);
    }
    return SUCCESS;
  }

  // override to add streaming
  WARN_UNUSED really_inline error_code finish() {
    if ( structurals.past_end(doc_parser().n_structural_indexes) ) {
      log_error("IMPOSSIBLE: past the end of the JSON!");
      return on_error(TAPE_ERROR);
    }
    end_document(0, 1);
    if (depth != 0) {
      log_error("Unclosed objects or arrays!");
      return on_error(TAPE_ERROR);
    }
    bool finished = structurals.at_end(doc_parser().n_structural_indexes);
    if (!finished) { log_value("(and has more)"); }
    return on_success(finished ? SUCCESS : SUCCESS_AND_HAS_MORE);
  }
};

} // namespace stage2

/************
 * The JSON is parsed to a tape, see the accompanying tape.md file
 * for documentation.
 ***********/
WARN_UNUSED error_code implementation::stage2(const uint8_t *buf, size_t len, parser &doc_parser, size_t &next_json) const noexcept {
  doc_parser.parsing_buf = buf;
  stage2::streaming_structural_parser parser(len, doc_parser);
  error_code result = parser.start(len);
  if (result) { return result; }

  if (parser.parse_root_value()) {
    return parser.error();
  }
  next_json = doc_parser.next_structural;
  return parser.finish();
}
