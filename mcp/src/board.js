/**
 * Faraday board widget — the MCP App.
 *
 * A list of findings in text is usable; a board with the findings ON it is what makes an FAE
 * trust the tool (ABT #665). So this is a packaging exercise, not new rendering code: it
 * mounts the web app's own BoardView over the same report JSON the CLI writes, and adds the
 * one thing a chat needs that the browser app does not — reporting the engineer's chosen
 * finding back to the model, so the next question can be about that finding.
 *
 * Importing BoardView rather than redrawing means the board in a chat and the board in the
 * browser cannot disagree about what the copper looks like.
 */
import { createApp, defineComponent, h, ref, computed } from "vue";
import { App } from "@modelcontextprotocol/ext-apps";
import BoardView from "../../web/src/components/BoardView.vue";

const app = new App({ name: "Faraday board", version: "0.1.0" });

const SEVERITY_ORDER = { high: 0, medium: 1, low: 2, info: 3 };

const Widget = defineComponent({
  setup() {
    const report = ref(null);
    const selectedId = ref("");
    const error = ref("");
    const counts = ref({});
    const review = ref("");
    const dropped = ref([]);

    // TWO VOCABULARIES, ONE SET. The payload is a `findings` result under the pipeline
    // contract: `findings` is the contract projection — severity as a label, nets by NAME,
    // numbers as {value, unit} — and `subject.document` is the engine's own report, which is
    // what BoardView draws from. They are the same findings in the same order, built from one
    // list server-side, so the drawing cannot disagree with the list beside it.
    const findings = ref([]);                       // contract shape — the list, the selection
    const drawn = computed(() => report.value?.findings ?? []);   // engine shape — the drawing
    const ordered = computed(() => [...findings.value].sort(
      (a, b) => (SEVERITY_ORDER[a.severity] ?? 9) - (SEVERITY_ORDER[b.severity] ?? 9)));

    /** The nets a finding is about, by name. The engine references them by index. */
    const netsOf = (f) => (f.involves ?? []).filter((i) => i.kind === "net").map((i) => i.name);

    /** Report the engineer's choice to the model. */
    async function choose(id) {
      selectedId.value = id;
      const f = findings.value.find((x) => x.id === id);
      if (!f) return;
      const where = netsOf(f).join(" <-> ");
      // updateModelContext OVERWRITES, so the message restates what was being looked at —
      // otherwise the model gets a finding id with no board and no question behind it.
      await app.updateModelContext({
        content: [{
          type: "text",
          text: [
            `[user selected] ${f.id} — ${f.rule}, severity ${f.severity}.`,
            `[what] ${f.summary}`,
            where ? `[nets] ${where}` : null,
            `[confidence] ${f.confidence}`,
            f.remediation ? `[remediation as given] ${f.remediation}` : null,
            `[review] ${review.value}`,
          ].filter(Boolean).join("\n"),
        }],
        structuredContent: JSON.parse(JSON.stringify({
          selected: {
            id: f.id, rule: f.rule, severity: f.severity, summary: f.summary,
            nets: netsOf(f), confidence: f.confidence ?? null, metrics: f.metrics ?? {},
          },
          context: { review: review.value },
        })),
      });
    }

    app.ontoolresult = async (result) => {
      const sc = result?.structuredContent;
      const document = sc?.subject?.document;
      if (sc?.mode !== "findings" || !document) {
        error.value = "The tool returned no board for this widget.";
        return;
      }
      report.value = document;
      findings.value = sc.findings ?? [];
      counts.value = sc.counts ?? {};
      review.value = sc.review ?? "";
      dropped.value = sc.dropped ?? [];
      // explain_finding returns exactly one, and the point of that call is to look at it.
      selectedId.value = findings.value.length === 1 ? findings.value[0].id : "";
    };

    return () => {
      if (error.value) return h("div", { class: "err" }, error.value);
      if (!report.value) return h("div", { class: "muted pad" }, "Waiting for a board…");

      const tally = Object.entries(counts.value)
        .filter(([, n]) => n)
        .map(([s, n]) => h("span", { class: `chip ${s}` }, `${n} ${s}`));

      // What the screen found and did NOT show. A widget that renders 200 of 428 findings
      // without saying so describes less than half the board and looks complete doing it.
      const omitted = dropped.value.reduce((n, d) => n + (d.count ?? 0), 0);

      return h("div", { class: "wrap" }, [
        h("div", { class: "head" }, [
          h("h1", {}, `${findings.value.length} finding${findings.value.length === 1 ? "" : "s"}`),
          h("div", { class: "chips" }, tally),
          h("div", { class: "sub" },
            "Click a finding on the board or in the list — your choice goes back to the assistant."),
          omitted
            ? h("div", { class: "sub omitted" },
                `${omitted} more not shown: ${dropped.value.map((d) => `${d.count} ${d.reason}`).join("; ")}.`)
            : null,
        ]),
        h("div", { class: "split" }, [
          h("div", { class: "boardpane" }, [
            h(BoardView, {
              report: report.value,
              findings: drawn.value,
              selectedId: selectedId.value,
              onSelect: (id) => choose(id),
            }),
          ]),
          h("ul", { class: "list" }, ordered.value.slice(0, 60).map((f) =>
            h("li", {
              class: `item ${f.severity}${f.id === selectedId.value ? " chosen" : ""}`,
              onClick: () => choose(f.id),
            }, [
              h("div", { class: "itemhead" }, [
                h("span", { class: "fid" }, f.id),
                h("span", { class: `sev ${f.severity}` }, f.severity),
                h("span", { class: "rule" }, f.rule),
              ]),
              h("div", { class: "title" }, f.summary),
              netsOf(f).length ? h("div", { class: "nets" }, netsOf(f).join(" ↔ ")) : null,
            ]))),
        ]),
      ]);
    };
  },
});

createApp(Widget).mount("#app");
await app.connect();
