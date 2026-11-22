// out.fail stops the post; the error names the line of the call. A catch around it cannot swallow it.
definePost(testPost({
  onSectionStart(section, ctx, out) {
    if (section.tool.lengthOffset !== 99) {
      try {
        out.fail("tool " + section.tool.number + " needs length offset 99"); /*HERE*/
      } catch (e) {
        out.raw("caught: " + e.message);
      }
      out.raw("still running");
    }
  },
}));
