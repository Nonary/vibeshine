/**
 * @brief Add a button to open the configuration option for each table
 */
document.addEventListener("DOMContentLoaded", function() {
  const tables: NodeListOf<HTMLTableElement> = document.querySelectorAll("table");
  tables.forEach(table => {
    if (table.className !== "doxtable") {
      return;
    }

    let previousElement: Element | null = table.previousElementSibling;
    while (previousElement && previousElement.tagName !== "H2") {
      previousElement = previousElement.previousElementSibling;
    }
    if (previousElement && previousElement.textContent) {
      const sectionId: string = previousElement.textContent.trim().toLowerCase();
      const newRow: HTMLTableRowElement = document.createElement("tr");

      const newCell: HTMLTableCellElement = document.createElement("td");
      newCell.setAttribute("colspan", "3");

      const newCode: HTMLElement = document.createElement("code");
      newCode.className = "open-button";
      newCode.setAttribute("onclick", `window.open('https://${(document.getElementById('host-authority') as HTMLInputElement).value}/config/#${sectionId}', '_blank')`);
      newCode.textContent = "Open";

      newCell.appendChild(newCode);
      newRow.appendChild(newCell);

      // get the table body
      const tbody: HTMLTableSectionElement | null = table.querySelector("tbody");

      // Insert at the beginning of the table
      if (tbody) {
        tbody.insertBefore(newRow, tbody.firstChild);
      }
    }
  });
});