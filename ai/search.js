const { GoogleGenerativeAI } = require("@google/generative-ai");

// Initialize the Gemini client using the Railway environment variable
const genAI = new GoogleGenerativeAI(process.env.GEMINI_API_KEY);

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
  try {
    // We use gemini-1.5-flash for speed, and force the output to be JSON
    const model = genAI.getGenerativeModel({
      model: "gemini-1.5-flash",
      generationConfig: { 
        responseMimeType: "application/json" 
      },
      systemInstruction: SYSTEM_PROMPT
    });

    const userMessage = `Library contents:\n${bookContext}\n\nUser query: "${query}"`;

    const result = await model.generateContent(userMessage);
    const raw = result.response.text();

    return JSON.parse(raw);
  } catch (err) {
    console.error("[Gemini API Error]:", err);
    throw err;
  }
}

module.exports = { searchBooks };