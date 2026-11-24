// A probe of the device's path rules for testPathRules.sh, which holds the fake's rules to them. It
// reads one request field per line on stdin. For each field it prints the verdict of every check a
// verb puts it through, in this order:
// `<ls> <read> <target> <sd-only> <write> <tree moved in> <removal> <write, system>
// <removal, system>`. A verdict is `ok`, the refusal word, or `-` where an earlier check already
// refused. <read> is a get's check and the rest are a change's; the last two carry the request's
// word `system`.
#include "codec.h"
#include "path.h"

#include <stdio.h>
#include <string.h>

static const char *verdict(const char *refusal) {
	return refusal ? refusal : "ok";
}

int main(void) {
	char field[1024];
	while (fgets(field, sizeof(field), stdin)) {
		field[strcspn(field, "\n")] = '\0';
		Target t;
		bool parsed = targetParse(field, &t);
		const char *read = "ok", *target = "ok"; // cmdFs.c's argTarget
		if (!parsed || strcmp(t.path, "/") == 0) {
			read = target = "path";
		} else if (t.kind == TARGET_SD) {
			read = pathProtected(t.path, true) ? "protected" : "ok";
			target = pathProtected(t.path, false) ? "protected" : "ok";
		}
		bool targetOk = strcmp(target, "ok") == 0, sd = targetOk && t.kind == TARGET_SD;
		printf("%s %s %s %s %s %s %s %s %s\n", parsed ? "ok" : "path", read, target,
			   targetOk && !sd ? "path" : target,
			   targetOk ? verdict(pathWriteRefusal(&t, false, false)) : "-",
			   sd ? verdict(pathWriteRefusal(&t, true, false)) : "-",
			   sd ? verdict(pathRemoveRefusal(t.path, false)) : "-",
			   targetOk ? verdict(pathWriteRefusal(&t, false, true)) : "-",
			   sd ? verdict(pathRemoveRefusal(t.path, true)) : "-");
	}
	return 0;
}
