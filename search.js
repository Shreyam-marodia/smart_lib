const Anthropic = require("@anthropic-ai/sdk");

const client = new Anthropic({ apiKey: process.env.ANTHROPIC_API_KEY });

const SYSTEM_PROMPT = `You are a smart library assistant. Your job is to help users find books in a home library.

Given a user query (by title, author, genre, or vague description) and a list of books in the library, 
you must identify the best matching book(s).

The book list will be provided in this format:
ID:<id> | "<title>" by <author> | Genre: <genre> | <shelf> Row <row> Col <col> | LED:<led_index>

Rules:
- Match fuzzily: partial titles, misspelled names, genre descriptions all count
- Return up to 3 best matches, ranked by confidence
- If the user asks for a genre or "something like X", return the best genre matches
- If no books match at all, say so clearly
- Always respond with valid JSON only, no markdown, no explanation outside JSON

Response format:
{
  "matches": [
    {
      "id": <book_id>,
      "title": "<title>",
      "author": "<author>",
      "genre": "<genre>",
      "led_index": <number>,
      "shelf_name": "<shelf>",
      "shelf_row": <number>,
      "shelf_col": <number>,
      "confidence": "high" | "medium" | "low",
      "reason": "<why this matched>"
    }
  ],
  "message": "<friendly response to the user>"
}

If no matches: { "matches": [], "message": "<explanation>" }`;

async function searchBooks(query, bookContext) {
  const userMessage = `Library contents:\n${bookContext}\n\nUser query: "${query}"`;

  const response = await client.messages.create({
    model: "claude-sonnet-4-6",
    max_tokens: 1000,
    system: SYSTEM_PROMPT,
    messages: [{ role: "user", content: userMessage }],
  });

  const raw = response.content[0].text.trim();

  try {
    return JSON.parse(raw);
  } catch {
    // Strip markdown fences if present
    const clean = raw.replace(/```json|```/g, "").trim();
    return JSON.parse(clean);
  }
}

module.exports = { searchBooks };
