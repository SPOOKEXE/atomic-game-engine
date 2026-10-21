/* Static reading aids; Doxygen retains ownership of search and clipboard behavior. */
(function () {
    "use strict";

    function unusedId(base) {
        let candidate = base;
        let suffix = 2;
        while (document.getElementById(candidate)) {
            candidate = base + "-" + suffix++;
        }
        return candidate;
    }

    function headingLabel(heading) {
        const copy = heading.cloneNode(true);
        copy.querySelectorAll("a.anchorlink").forEach(function (anchor) {
            anchor.remove();
        });
        return copy.textContent.replace(/\s+/g, " ").trim();
    }

    function headingId(heading) {
        const anchor = heading.querySelector("a.anchor[id]");
        return heading.id || (anchor && anchor.id) || "";
    }

    function addContents(textblock) {
        const contents = textblock.parentElement;
        if (contents.children.length !== 1 || contents.firstElementChild !== textblock ||
            textblock.querySelector(".docs-home") || contents.querySelector(".toc, .reading-toc")) {
            return;
        }
        const headings = Array.from(textblock.querySelectorAll("h1, h2"))
            .map(function (heading) {
                return { element: heading, label: headingLabel(heading), id: headingId(heading) };
            })
            .filter(function (heading) { return heading.label.length > 0; });
        if (headings.filter(function (heading) { return heading.id.length > 0; }).length < 4) {
            return;
        }

        const details = document.createElement("details");
        details.className = "reading-toc";
        const summary = document.createElement("summary");
        summary.id = unusedId("reading-contents-title");
        summary.textContent = "On this page";
        details.appendChild(summary);
        const navigation = document.createElement("nav");
        navigation.setAttribute("aria-labelledby", summary.id);
        const list = document.createElement("ol");
        const hasTopLevel = headings.some(function (heading) { return heading.element.tagName === "H1"; });
        headings.forEach(function (heading) {
            if (!heading.id) {
                const slug = heading.label.toLowerCase().normalize("NFKD")
                    .replace(/[\u0300-\u036f]/g, "").replace(/[^a-z0-9]+/g, "-")
                    .replace(/^-|-$/g, "").slice(0, 80);
                heading.id = unusedId("section-" + (slug || "heading"));
                heading.element.id = heading.id;
            }
            const item = document.createElement("li");
            if (hasTopLevel && heading.element.tagName === "H2") {
                item.className = "reading-toc-subsection";
            }
            const link = document.createElement("a");
            link.href = "#" + encodeURIComponent(heading.id);
            link.textContent = heading.label;
            item.appendChild(link);
            list.appendChild(item);
        });
        navigation.appendChild(list);
        details.appendChild(navigation);
        contents.insertBefore(details, textblock);
        contents.classList.add("reading-layout");
        const wide = window.matchMedia("(min-width: 1200px)");
        details.open = wide.matches;
        wide.addEventListener("change", function (event) { details.open = event.matches; });
    }

    function isEditor(target) {
        return target instanceof Element && (target.isContentEditable ||
            target.closest("input, textarea, select, [contenteditable]:not([contenteditable='false']), [role='textbox'], .CodeMirror, .monaco-editor"));
    }

    function enableCopyButtons() {
        document.querySelectorAll("doxygen-awesome-fragment-copy-button").forEach(function (button) {
            if (button.dataset.readingAccessible) {
                return;
            }
            button.dataset.readingAccessible = "true";
            button.tabIndex = 0;
            button.setAttribute("role", "button");
            button.setAttribute("aria-label", "Copy code");
            button.querySelectorAll("svg").forEach(function (icon) {
                icon.setAttribute("aria-hidden", "true");
                icon.setAttribute("focusable", "false");
            });
            button.addEventListener("keydown", function (event) {
                if (event.key === "Enter" || event.key === " ") {
                    event.preventDefault();
                    if (!event.repeat) {
                        button.click();
                    }
                }
            });
        });
    }

    function initialise() {
        const content = document.getElementById("doc-content");
        if (!content || content.dataset.readingReady) {
            return;
        }
        content.dataset.readingReady = "true";
        if (!content.hasAttribute("tabindex")) {
            content.tabIndex = -1;
        }
        if (!document.querySelector("main, [role='main']")) {
            content.setAttribute("role", "main");
        }
        const skip = document.createElement("a");
        skip.className = "reading-skip-link";
        skip.href = "#doc-content";
        skip.textContent = "Skip to content";
        skip.addEventListener("click", function () { content.focus({ preventScroll: true }); });
        document.body.insertBefore(skip, document.body.firstChild);
        document.querySelectorAll(".contents > .textblock").forEach(addContents);
        document.addEventListener("keydown", function (event) {
            if (event.key !== "/" || event.defaultPrevented || event.ctrlKey || event.altKey ||
                event.metaKey || event.isComposing || isEditor(event.target)) {
                return;
            }
            const search = Array.from(document.querySelectorAll("[id='MSearchField']")).find(function (candidate) {
                return !candidate.disabled && !candidate.readOnly && !candidate.closest("[hidden], [inert]") &&
                    candidate.getClientRects().length > 0 && window.getComputedStyle(candidate).visibility === "visible";
            });
            if (!search) {
                return;
            }
            event.preventDefault();
            search.focus();
        });
        enableCopyButtons();
    }

    // The vendor installs its copy controls in an earlier DOMContentLoaded listener.
    if (document.readyState === "loading" || document.readyState === "interactive") {
        document.addEventListener("DOMContentLoaded", initialise, { once: true });
    } else {
        initialise();
    }
    // Also handles scripts inserted after DOMContentLoaded, without polling the document.
    window.addEventListener("load", function () {
        initialise();
        enableCopyButtons();
    }, { once: true });
}());
