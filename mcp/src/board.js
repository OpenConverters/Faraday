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
// THE PALETTE, or the board draws in nothing. BoardView paints to a canvas, and a
// canvas cannot inherit CSS — theme.js reads ~30 tokens back with getComputedStyle
// and hands them to fillStyle. Without this import every one of those reads
// returned "", Canvas 2D silently IGNORES an invalid fillStyle, and the board came
// out dark on black with no layers and every finding the same colour. Tokens only,
// not the web app's style.css: that file's `body`/`*` rules would fight this
// widget's own shell.
import "../../web/src/tokens.css";

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
    //
    // KNOWING THAT AND SPEAKING ONLY ONE OF THEM was the bug. The list is fed from
    // the CONTRACT shape when the payload is complete and from the ENGINE report
    // when it is truncated — and on any real board it is truncated, because a
    // 109-finding review reports 15. Everything below then read the wrong field on
    // the shape it actually had:
    //
    //   contract          engine report
    //   severity: "high"  severity: 0.58  (a score) + severityLabel: "high"
    //   summary           title
    //   involves[]        netA / netB, as INDICES into board.nets
    //
    // `class="item ${f.severity}"` became `item 0.5846643665323331`, which matches
    // no rule, so the severity border was transparent and the coloured word was a
    // raw float — the severity of every issue invisible on exactly the boards that
    // have enough issues to need sorting. The sort was equally blind: every
    // SEVERITY_ORDER lookup missed, so `high` and `info` came back in report order.
    //
    // So normalise once, at the boundary, and let one vocabulary out of it.
    const findings = ref([]);                       // NORMALISED shape — the list, the selection
    const drawn = computed(() => report.value?.findings ?? []);   // engine shape — the drawing

    /** Net names by index, for resolving the engine's netA/netB. */
    const netNames = computed(() =>
      (report.value?.board?.nets ?? []).map((n) => n?.name ?? ""));

    /**
     * One finding, in one vocabulary, whichever shape it arrived in.
     *
     * Detection is on `severityLabel`, the field only the engine report has —
     * NOT on typeof severity, because "is this a number?" would quietly mis-read a
     * contract payload whose severity was ever numeric, and because a field that
     * exists in exactly one of the two shapes is an unambiguous witness.
     */
    function normalise(f) {
      if (f.severityLabel === undefined) return f;          // already the contract shape
      const names = netNames.value;
      const nets = ["netA", "netB"]
        .map((k) => f[k])
        .map((i) => (typeof i === "number" ? (i >= 0 ? names[i] : null) : i || null))
        .filter(Boolean);
      return {
        ...f,
        severity: f.severityLabel,
        // The score is not thrown away — it is what the engine ranked on, and the
        // tooltip is the honest place for it.
        score: typeof f.severity === "number" ? f.severity : null,
        summary: f.title ?? f.rule ?? f.id,
        involves: nets.map((name) => ({ kind: "net", name })),
      };
    }

    const ordered = computed(() => [...findings.value].sort(
      (a, b) => (SEVERITY_ORDER[a.severity] ?? 9) - (SEVERITY_ORDER[b.severity] ?? 9)
                || (b.score ?? 0) - (a.score ?? 0)));

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
      if (sc?.mode !== "findings") {
        error.value = "The tool returned no board for this widget.";
        return;
      }
      // The report does NOT ride in the payload any more: on a real board it is 860,566
      // characters, and a tool result that size is refused before the model sees any of it.
      // The widget is the only party that needs the copper, so the widget asks for it.
      let document = sc?.subject?.document;
      if (!document) {
        const ref = sc.subject?.reviewRef || sc.review;
        if (!ref) {
          error.value = "The tool returned no board for this widget.";
          return;
        }
        try {
          // callServerTool, not callTool: the widget asks its SERVER for a tool; callTool is
          // the host bridge's own method, and calling it here fails with the unhelpful
          // "io.callTool is not a function".
          const got = await app.callServerTool({ name: "fetch_report",
                                                 arguments: { review: ref } });
          document = got?.structuredContent?.document;
        } catch (err) {
          error.value = `Could not load the board for review ${ref}: ${err.message}`;
          return;
        }
        if (!document) {
          error.value = `Review ${ref} returned no report to draw.`;
          return;
        }
      }
      report.value = document;
      // The payload's findings may be the TOP of a longer list — review_board names five on a
      // board with two hundred, because two hundred will not fit in a result the model can be
      // given. counts describes the review, `reported` this payload: when they disagree, the
      // fetched report is the complete set and is what belongs on the copper.
      const total = Object.values(sc.counts ?? {}).reduce((a, b) => a + b, 0);
      const truncated = total > (sc.reported ?? (sc.findings ?? []).length);
      // Normalised at the boundary — see normalise(). report.value is assigned
      // above, so netNames is already resolvable when the engine shape arrives.
      findings.value = (truncated ? (document.findings ?? sc.findings ?? [])
                                  : (sc.findings ?? [])).map(normalise);
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
                // The engine's numeric score rides in the tooltip: it is what the
                // ranking used, and hiding it entirely would make two findings of the
                // same label look interchangeable when the list order says they are not.
                h("span", { class: `sev ${f.severity}`,
                            title: f.score !== null && f.score !== undefined
                              ? `severity score ${Number(f.score).toFixed(3)}` : undefined },
                  f.severity),
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

/**
 * Wear the skin the HOST is wearing.
 *
 * A widget lives on an opaque origin in a sandboxed iframe: it cannot see the page
 * around it, and `prefers-color-scheme` inside it answers for the MACHINE — which
 * is the wrong answer the moment the reader picks the theme the machine is not
 * wearing. The MCP host sends its theme as host context for exactly this reason,
 * and Moebius has been sending it all along with nothing in the pool reading it.
 *
 * Setting `data-theme` is all it takes: tokens.css keys the light palette off
 * `:root[data-theme="light"]`, and theme.js's own MutationObserver watches that
 * same attribute — so the board repaints itself with no extra wiring here. It also
 * carries `color-scheme`, which palette() reads to decide which way "hotter"
 * points on the risk ramp: white-hot over a dark board, ember-dark over a pale one.
 */
function wearHostTheme(theme) {
  // Default to dark rather than to the machine: dark is what tokens.css defines on
  // bare :root, so an absent or unknown value lands on a palette that exists.
  document.documentElement.dataset.theme = theme === "light" ? "light" : "dark";
}

createApp(Widget).mount("#app");
// Handler before connect(): the host may push context during the handshake, and a
// late listener misses it — the same reason ontoolresult is registered up top.
app.onhostcontextchanged = (ctx) => wearHostTheme(ctx?.theme);
await app.connect();
wearHostTheme(app.getHostContext()?.theme);
