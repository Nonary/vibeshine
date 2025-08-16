/**
 * @brief Add a button to open the configuration option for each table
 */
document.addEventListener("DOMContentLoaded", function(): void {
  const tables = document.querySelectorAll("table");
  tables.forEach((table: Element) => {
    if (table.className !== "doxtable") {
      return;
    }

    let previousElement: Element | null = table.previousElementSibling;
    while (previousElement && previousElement.tagName !== "H2") {
      previousElement = previousElement.previousElementSibling;
    }
    
    if (previousElement && previousElement.textContent) {
      const sectionId = previousElement.textContent.trim().toLowerCase();
      const newRow = document.createElement("tr");

      const newCell = document.createElement("td");
      newCell.setAttribute("colspan", "3");

      const newCode = document.createElement("code");
      newCode.className = "open-button";
      const hostAuthority = document.getElementById('host-authority') as HTMLInputElement;
      const hostValue = hostAuthority ? hostAuthority.value : 'localhost';
      newCode.setAttribute("onclick", `window.open('https://${hostValue}/config/#${sectionId}', '_blank')`);
      newCode.textContent = "Open";

      newCell.appendChild(newCode);
      newRow.appendChild(newCell);

      // get the table body
      const tbody = table.querySelector("tbody");
      if (tbody) {
        // Insert at the beginning of the table
        tbody.insertBefore(newRow, tbody.firstChild);
      }
    }
  });
});