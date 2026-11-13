# vu1bin.awk -v name=NAME NAME.h: the binary of a VU1 vertex program for
# glShaderBinary (GL_PS2_VU1_BINARY, <mxgl.h>) from its layout header: the
# cbuffer's members (the header's VU_NAME_<MEMBER> before ENTRY), their
# sizes (to the next member, the last to UNI_QW), the inputs' semantics.
BEGIN {
  up = toupper(name); pre = "VU_" up "_"
  split("ENTRY BUF_QW UNI_QW CTX_AT CTX_QW NCTX ITEM_QW OUT_QW MAX_N ALIGN COUNTS IN_AT OUT_AT NREG REGS NIN INPUTS ROW MAX_I IDX_AT SPILL_QW", k, " ")
  for (i in k) known[k[i]] = 1
  nu = 0; ni = 0
}
$1 == "#define" && index($2, pre) == 1 {
  key = substr($2, length(pre) + 1)
  if (key == "UNI_QW") uniqw = $3 + 0
  if (substr(key, 1, 3) == "IN_" && key != "IN_AT") { inn[$3 + 0] = substr(key, 4); if ($3 + 1 > ni) ni = $3 + 1; next }
  if (key in known) next
  un[nu] = tolower(key); ua[nu] = $3 + 0; nu++
}
END {
  # by address
  for (i = 0; i < nu; i++) for (j = i + 1; j < nu; j++) if (ua[j] < ua[i]) {
    t = ua[i]; ua[i] = ua[j]; ua[j] = t; t = un[i]; un[i] = un[j]; un[j] = t }
  print "/* generated from " name ".h by vu1bin.awk */"
  print "#include <mx/frame.h>"
  print "#include <mxgl.h>"
  print "#include \"" name ".h\""
  print "extern const u8 " name "_vu[], " name "_vu_end[];"
  print "static void shader(void *s) { *(struct mx_shader *)s = MX_VU_SHADER(" up ", " name "_vu, " name "_vu_end); }"
  printf "static const struct mxgl_vu1_uniform uni[] = {"
  for (i = 0; i < nu; i++) printf "%s{\"%s\", %d, %d}", (i ? ", " : ""), un[i], ua[i], (i + 1 < nu ? ua[i + 1] : uniqw) - ua[i]
  if (!nu) printf "{0, 0, 0}"
  print "};"
  printf "static const char *const in[] = {"
  for (i = 0; i < ni; i++) printf "%s\"%s\"", (i ? ", " : ""), inn[i]
  print "};"
  print "const struct mxgl_vu1_binary " name "_vu1 = {MXGL_VU1_MAGIC, shader, " nu ", uni, " ni ", in};"
}
