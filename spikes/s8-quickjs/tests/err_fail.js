definePost({
  onSectionStart(section, ctx, out) {
    if (section.tool.lengthOffset === undefined) {
      out.fail("tool " + section.tool.number + " has no length offset"); /*HERE*/
    }
  },
});
