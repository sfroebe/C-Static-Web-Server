const button = document.querySelector("#message-button");
const message = document.querySelector("#message");

button.addEventListener("click", () => {
    message.textContent = "JavaScript loaded from ./public/app.js.";
});
